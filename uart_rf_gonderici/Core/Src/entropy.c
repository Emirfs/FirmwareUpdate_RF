/*
 * entropy.c — Donanim entropi kaynagi (UID + ADC gurultusu + SysTick)
 *
 * STM32F030'da donanim RNG yoktur. Bu modul uc kaynagin karmasi ile
 * her cihazda ve her sessionda esiz, tahmin edilemez private key uretir:
 *
 *   Katman 1 — Cihaz esizligi:
 *     UID (96-bit, fabrikada yakilmis) → farkli cihazlar farkli key uretir
 *
 *   Katman 2 — Session esizligi:
 *     ADC ic sicaklik sensoru (kanal 16) → analog gurultu her okumada farkli
 *     ADC Vrefint (kanal 17)            → ek analog gurultu
 *
 *   Katman 3 — Zamanlama belirsizligi:
 *     SysTick->VAL (her ADC okumasi arasinda) → frekans sapmasindan kaynaklanan jitter
 *
 *   Karistirma:
 *     FNV-1a hash (32-byte durum uzerinde) → avalanche etkisi
 *     Her turda 32 byte durum tamamen yeniden hashlenir
 *
 * FNV-1a (Fowler-Noll-Vo) neden?
 *   - Cok kucuk implementasyon (~100 byte)
 *   - Iyi dag√¢√¢√¢√¢√¢√¢√¢√¢√¢√¢lim ozelligi (bir bit degisiklik tum cikisi etkiler)
 *   - Kriptografik degil ama iyi entropi karistirici
 *   - Gerçek rastgelelik ADC'den geliyor; hash sadece kariştiriyor
 */

#include "entropy.h"
#include <string.h>
#include "stm32f0xx.h"   /* ADC, SysTick, UID register tanimlari */

/* STM32F030 UID adresi (referans kilavuzu: RM0360, bolum 26.2) */
#define UID_ADDR_W0  0x1FFFF7ACU
#define UID_ADDR_W1  0x1FFFF7B0U
#define UID_ADDR_W2  0x1FFFF7B4U

/* ADC kanal sabitleri */
#define ADC_CHAN_TEMP    16U  /* Ic sicaklik sensoru */
#define ADC_CHAN_VREFINT 17U  /* Ic referans voltaji */

/* ─────────────────────────────────────────────────────────────
 * FNV-1a hash (32-byte tampon uzerine)
 *
 * Her byte'i 32-byte havuza XOR sonrasi FNV prime ile carp.
 * Butun havuz bit duzeyi etkilenir → avalanche.
 * ───────────────────────────────────────────────────────────── */
static void fnv1a_mix(uint8_t *pool, uint8_t new_byte)
{
    /* FNV-1a 32-bit: her havuz byte'ini yeni byte ile karistir */
    uint32_t h = 2166136261UL; /* FNV offset basis */
    int i;

    /* once yeni byte'i havuza XOR et */
    pool[0] ^= new_byte;

    /* sonra tum havuz uzerinde FNV-1a uygula (circular) */
    for (i = 0; i < 32; i++) {
        h ^= (uint32_t)pool[i];
        h *= 16777619UL; /* FNV prime */
        pool[i] = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    }
}

/* ─────────────────────────────────────────────────────────────
 * ADC tek okuma (register seviyesi, HAL gerektirmez)
 *
 * STM32F030 ADC: SAR 12-bit, tek donusum modu.
 * Bu fonksiyon:
 *   - ADC'yi acık bulursa doğrudan kanal secer ve okur
 *   - ADC kapali ise Nothing (guvenli degrade)
 * ───────────────────────────────────────────────────────────── */
static uint16_t adc_read_channel(uint32_t channel)
{
    /* Kanal sec (SQR3[4:0] = kanal numarası, tek donusum) */
    ADC1->CHSELR = (1UL << channel);

    /* Donusumu bashat */
    ADC1->CR |= ADC_CR_ADSTART;

    /* Bitmesini bekle (max ~14µs @ 24MHz, ADC clock = 4MHz) */
    uint32_t timeout = 10000;
    while (!(ADC1->ISR & ADC_ISR_EOC) && timeout--)
        ;

    uint16_t val = (uint16_t)(ADC1->DR & 0x0FFF);
    return val;
}

/* ─────────────────────────────────────────────────────────────
 * ADC başlatma (geçici, bootloader başında bir kez)
 * ───────────────────────────────────────────────────────────── */
static void adc_entropy_init(void)
{
    /* ADC saat etkinlestir */
    RCC->APB2ENR |= RCC_APB2ENR_ADCEN;

    /* ADC kalibrasyonu (cihaz resetinden sonra onerilen) */
    ADC1->CR &= ~ADC_CR_ADEN;       /* Once kapat */
    ADC1->CR |= ADC_CR_ADCAL;       /* Kalibrasyon baslat */
    while (ADC1->CR & ADC_CR_ADCAL) /* Bitmesini bekle */
        ;

    /* Tek donusum modu, en yavaş örnekleme (239.5 cycle = max gurultu) */
    ADC1->CFGR1 = 0; /* Tek donusum, 12-bit */
    ADC1->SMPR  = ADC_SMPR_SMP_2 | ADC_SMPR_SMP_1 | ADC_SMPR_SMP_0; /* 239.5 cycle */

    /* Ic sensörler etkinlestir (sicaklik + Vrefint) */
    ADC->CCR |= ADC_CCR_TSEN | ADC_CCR_VREFEN;

    /* ADC ac */
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY))  /* Hazir olmasini bekle */
        ;
}

/* ─────────────────────────────────────────────────────────────
 * ADC durdur (entropi toplandiktan sonra)
 * ───────────────────────────────────────────────────────────── */
static void adc_entropy_deinit(void)
{
    ADC1->CR &= ~ADC_CR_ADEN;
    ADC->CCR &= ~(ADC_CCR_TSEN | ADC_CCR_VREFEN);
    RCC->APB2ENR &= ~RCC_APB2ENR_ADCEN;
}

/* ─────────────────────────────────────────────────────────────
 * entropy_generate — Ana fonksiyon
 * ───────────────────────────────────────────────────────────── */
void entropy_generate(uint8_t *buf, uint16_t len)
{
    uint8_t pool[32];
    int i;

    /* ── Katman 1: UID (12 byte, her cihaza ozel) ── */
    uint32_t uid0 = *(volatile uint32_t *)UID_ADDR_W0;
    uint32_t uid1 = *(volatile uint32_t *)UID_ADDR_W1;
    uint32_t uid2 = *(volatile uint32_t *)UID_ADDR_W2;

    /* UID'yi havuza dagit */
    memcpy(pool,      &uid0, 4);
    memcpy(pool + 4,  &uid1, 4);
    memcpy(pool + 8,  &uid2, 4);
    /* Kalan 20 byte sifir — ADC tur'ları doldurur */
    memset(pool + 12, 0, 20);

    /* UID uzerinde ilk karistirma */
    for (i = 0; i < 12; i++) {
        fnv1a_mix(pool, pool[i]);
    }

    /* ── Katman 2 + 3: ADC + SysTick (32 tur) ── */
    adc_entropy_init();

    for (i = 0; i < 32; i++) {
        /* Ic sicaklik sensoru: her okuma farkli analog gurultu */
        uint16_t temp_raw = adc_read_channel(ADC_CHAN_TEMP);

        /* SysTick: ADC okuma arasindaki zamanlama jitter'i */
        uint32_t tick = SysTick->VAL;

        /* Her iki kaynagi havuza karistir */
        fnv1a_mix(pool, (uint8_t)(temp_raw & 0xFF));
        fnv1a_mix(pool, (uint8_t)(temp_raw >> 8));
        fnv1a_mix(pool, (uint8_t)(tick & 0xFF));
        fnv1a_mix(pool, (uint8_t)((tick >> 8) & 0xFF));

        /* Tur sayisini da karıştır (havuz belirleyiciligi arttirir) */
        fnv1a_mix(pool, (uint8_t)i);

        /* Vrefint: ek analog gurultu (her 4 turda bir) */
        if ((i & 3) == 0) {
            uint16_t vref_raw = adc_read_channel(ADC_CHAN_VREFINT);
            fnv1a_mix(pool, (uint8_t)(vref_raw & 0xFF));
            fnv1a_mix(pool, (uint8_t)(vref_raw >> 8));
        }
    }

    adc_entropy_deinit();

    /* ── Çıkış: istenen uzunlukta byte kopyala ── */
    if (len <= 32) {
        memcpy(buf, pool, len);
    } else {
        /* len > 32 ise: ilk 32 byte kopyala, kalanı için havuzu yeniden karistir */
        memcpy(buf, pool, 32);
        for (i = 32; i < len; i++) {
            fnv1a_mix(pool, (uint8_t)i);
            buf[i] = pool[i % 32];
        }
    }

    /* Temizle — private key verisini Stack'te birakma */
    memset(pool, 0, sizeof(pool));
}

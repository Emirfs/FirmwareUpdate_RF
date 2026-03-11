/*
 * sender_fw_update.c — Firmware Update Ana Akış (Gönderici Tarafı)
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * PC'den gelen 'W' komutuyla başlayan firmware güncelleme sürecini yönetir.
 * Protokol adımları sırasıyla:
 *   1. Alıcıya RF üzerinden BOOT_REQUEST gönderir → BOOT_ACK bekler
 *      BOOT_ACK payload'ında alıcı kaldığı yeri bildirir (resume_start_packet)
 *   2. PC'den 12 byte metadata alır → RF ile alıcıya gönderir
 *   3. Alıcının hazır olmasını bekler (FLASH_ERASE_DONE — artık anında gelir)
 *   4. PC'den 148 byte'lık paketler alır (IV+Encrypted+CRC) →
 *      İlk resume_start_packet kadar paketi PC'den alır ama RF'e göndermez
 *      (alıcı zaten yazmış — sadece PC'ye ACK verir, yeniden göndermez).
 *      Kalan paketleri 4 RF DATA_CHUNK parçasına bölerek güvenilir gönderir.
 *   5. Alıcıdan UPDATE_COMPLETE / UPDATE_FAILED bekler → PC'ye sonucu bildirir
 *
 * ─── DEĞİŞTİRİLEBİLECEK ŞEYLER ───────────────────────────────────────────
 * - Boot ACK bekleme süresi: while döngüsündeki 30000 ms
 * - Flash silme bekleme süresi: while döngüsündeki 60000 ms
 * - Final sonuç bekleme süresi: while döngüsündeki 45000 ms
 * - Metadata retry sayısı: RF_MAX_RETRIES (rf_protocol.h)
 * - UART paket bekleme süresi: HAL_UART_Receive'deki 8000 ms
 *
 * ─── BAĞIMLILIKLAR ────────────────────────────────────────────────────────
 * sender_rf_link.c  → RF_SendPacket, RF_WaitForPacket, RF_SendChunkReliable
 * sender_state.h    → fw_packet_buf, rf_seq_counter
 * rf_protocol.h     → RF_CMD_xxx, RF_CHUNKS_PER_PACKET, UART_ACK/NACK
 */

#include "sender_fw_update.h"

#include "iwdg.h"
#include "rf_protocol.h"
#include "sender_rf_link.h"
#include "sender_state.h"
#include "sender_uart_debug.h"
#include "usart.h"
#include <string.h>

void FirmwareUpdate_Mode(void) {
  uint8_t ack = UART_ACK;   // PC'ye başarı bildirimi (0x06)
  uint8_t nack = UART_NACK; // PC'ye hata bildirimi (0x15)
  uint8_t rx_type, rx_pld[RF_MAX_DATA];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  /* Kaldigi yerden devam noktasi: alici BOOT_ACK payload'inda bildirir. */
  uint32_t resume_start_packet = 0;

  /* Toplam paket sayisi — metadata'dan okunduktan sonra doldurulur. */
  uint32_t total_fw_packets = 0;

  /* FW update sırasında debug print'i kapat — UART meşgul olacak */
  SenderDebug_SetEnabled(0);

  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET); // LED0 yak: FW update aktif

  /* ===================================================================
   * ADIM 0: KİMLİK DOĞRULAMA (NONCE CHALLENGE-RESPONSE)
   * =================================================================== */
  Print("[FW] Auth baslatiliyor...\r\n");

  {
    uint8_t  auth_pkt[52]; /* PC'den: IV(16)+Encrypted(32)+CRC32(4) */
    uint8_t  nonce_buf[4];
    uint8_t  challenge_got = 0;
    uint32_t auth_start = HAL_GetTick();

    /* AUTH_REQUEST gonder, AUTH_CHALLENGE (nonce) gelene kadar bekle */
    while (!challenge_got && (HAL_GetTick() - auth_start) < 15000) {
      HAL_IWDG_Refresh(&hiwdg);
      RF_SendPacket(RF_CMD_AUTH_REQUEST, rf_seq_counter, NULL, 0);

      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
        if (rx_type == RF_CMD_AUTH_CHALLENGE && rx_pld_len >= 4) {
          memcpy(nonce_buf, rx_pld, 4);
          challenge_got = 1;
        }
      }
    }

    if (!challenge_got) {
      Print("[FW] HATA: Auth challenge alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* PC'ye once ACK gonder ('W' komutuna cevap), sonra nonce'u gonder */
    HAL_UART_Transmit(&huart1, &ack, 1, 100);
    /* Nonce'u (4 byte) PC'ye gonder — PC auth paketini olusturacak */
    HAL_UART_Transmit(&huart1, nonce_buf, 4, 500);

    /* PC'den 52-byte auth paketi bekle */
    if (HAL_UART_Receive(&huart1, auth_pkt, 52, 15000) != HAL_OK) {
      Print("[FW] HATA: Auth paketi alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* CRC dogrula (CRC-32/ISO-HDLC: auth_pkt[0..47] icin) */
    uint32_t recv_crc, calc_crc;
    memcpy(&recv_crc, &auth_pkt[48], 4);
    calc_crc = 0xFFFFFFFF;
    for (int ci = 0; ci < 48; ci++) {
      calc_crc ^= (uint32_t)auth_pkt[ci];
      for (int b = 0; b < 8; b++) {
        if (calc_crc & 1) calc_crc = (calc_crc >> 1) ^ 0xEDB88320U;
        else              calc_crc >>= 1;
      }
    }
    calc_crc ^= 0xFFFFFFFF;

    if (calc_crc != recv_crc) {
      Print("[FW] HATA: Auth CRC gecersiz!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* RF_CMD_AUTH gonder: IV(16)+Encrypted(32) = 48 byte */
    uint8_t auth_ok = 0;
    for (uint8_t retry = 0; retry < 5 && !auth_ok; retry++) {
      RF_SendPacket(RF_CMD_AUTH, rf_seq_counter, auth_pkt, 48);

      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
        if (rx_type == RF_CMD_AUTH_ACK) {
          auth_ok = 1;
        } else if (rx_type == RF_CMD_AUTH_NACK) {
          Print("[FW] HATA: Alici auth reddetti!\r\n");
          HAL_UART_Transmit(&huart1, &nack, 1, 100);
          goto fw_update_cleanup;
        }
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;

    if (!auth_ok) {
      Print("[FW] HATA: Auth ACK alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* Auth basarili — PC'ye ACK ver */
    HAL_UART_Transmit(&huart1, &ack, 1, 100);
    Print("[FW] Auth basarili!\r\n");
  }

  Print("[FW] Aliciya BOOT_REQUEST gonderiliyor...\r\n");

  /* ===================================================================
   * ADIM 1: BOOT_REQUEST -> BOOT_ACK
   * Alici cihazi bootloader moduna almak icin BOOT_REQUEST gonder.
   * Alici BOOT_ACK donene veya 30 saniye gecene kadar tekrarla.
   *
   * BOOT_ACK payload (4 byte, little-endian): resume_start_packet
   *   - 0  : ilk transfer, bastan basla
   *   - N>0: onceki transfer N. pakete kadar basariliydi, N'den devam et
   * =================================================================== */
  uint8_t boot_ack_received = 0;
  uint32_t boot_start = HAL_GetTick();

  while (!boot_ack_received && (HAL_GetTick() - boot_start) < 30000) {
    HAL_IWDG_Refresh(&hiwdg); // Watchdog'u sifirla — loop uzun surebilir

    RF_SendPacket(RF_CMD_BOOT_REQUEST, rf_seq_counter, NULL, 0); // BOOT_REQUEST gonder

    /* 2 saniye BOOT_ACK bekle */
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
      if (rx_type == RF_CMD_BOOT_ACK) {
        boot_ack_received = 1;

        /* BOOT_ACK payload'indan resume noktasini oku (4 byte, little-endian) */
        if (rx_pld_len >= 4) {
          resume_start_packet = (uint32_t)rx_pld[0]
                              | ((uint32_t)rx_pld[1] << 8)
                              | ((uint32_t)rx_pld[2] << 16)
                              | ((uint32_t)rx_pld[3] << 24);
        }

        if (resume_start_packet > 0) {
          Print("[FW] Resume: ");
          PrintHex((uint8_t)(resume_start_packet >> 8));
          PrintHex((uint8_t)(resume_start_packet & 0xFF));
          Print(" paket zaten yazilmis, devam ediliyor\r\n");
        } else {
          Print("[FW] Alici bootloader'a gecti! (bastan baslaniyor)\r\n");
        }
      }
    }

    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin); // Beklerken LED yanip sonsun
  }

  if (!boot_ack_received) {
    /* 30 saniye icinde BOOT_ACK gelmedi — PC'ye NACK gonder ve cik */
    Print("[FW] HATA: Alici bootloader'a gecilemedi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    goto fw_update_cleanup;
  }

  /* Alici hazir — PC'ye ACK gonder, PC metadata gondermesini bekliyor */
  HAL_UART_Transmit(&huart1, &ack, 1, 100);

  /* ===================================================================
   * ADIM 2: METADATA AL -> RF ILE GONDER
   * PC'den 12 byte metadata bekle:
   *   [firmware_size:4][firmware_version:4][firmware_crc32:4]
   * Alininca RF uzerinden aliciya ilet, ACK bekle.
   * =================================================================== */
  uint8_t meta_buf[12];
  if (HAL_UART_Receive(&huart1, meta_buf, 12, 10000) != HAL_OK) {
    Print("[FW] HATA: Metadata alinamadi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    goto fw_update_cleanup;
  }

  Print("[FW] Metadata alindi, RF ile gonderiliyor...\r\n");

  uint8_t meta_sent = 0;
  uint32_t meta_start = HAL_GetTick();
  /* Zaman bazli deneme: BOOT_ACK gibi alakasiz paketler retry'i tukettirmesin.
   * Alici bootloader henuz BOOT_ACK yayinliyor olabilir — biz metadata
   * gonderiyor, alici hazir oldugunda ACK donecek. 15 saniye sure ver. */
  while (!meta_sent && (HAL_GetTick() - meta_start) < 15000) {
    RF_SendPacket(RF_CMD_METADATA, rf_seq_counter, meta_buf, 12);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
      if (rx_type == RF_CMD_ACK) {
        meta_sent = 1;
      }
      /* Baska paket turu (BOOT_ACK vb.) → yoksay, devam et */
    }
    HAL_IWDG_Refresh(&hiwdg);
  }
  rf_seq_counter++;

  if (!meta_sent) {
    Print("[FW] HATA: Metadata gonderilemedi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    goto fw_update_cleanup;
  }

  /* Metadata iletildi — PC'ye ACK */
  HAL_UART_Transmit(&huart1, &ack, 1, 100);

  /* firmware_size'tan toplam paket sayisini hesapla */
  {
    uint32_t fw_size;
    memcpy(&fw_size, meta_buf, 4);
    total_fw_packets = (fw_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
  }

  /* ===================================================================
   * ADIM 3: ALICI HAZIR BİLDİRİMİ (FLASH_ERASE_DONE)
   * Onceki tasarimda burada alici tum Flash'i siliyordu (~555ms).
   * Artik alici Flash'i sayfa sayfa siliyor (her 128-byte yazimdan once),
   * bu yuzden FLASH_ERASE_DONE cok daha hizli geliyor.
   * =================================================================== */
  Print("[FW] Alicinin hazir olması bekleniyor...\r\n");

  uint8_t erase_done = 0;
  uint32_t erase_start = HAL_GetTick();

  while (!erase_done && (HAL_GetTick() - erase_start) < 60000) { // 60 sn timeout
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_FLASH_ERASE_DONE) {
        erase_done = 1;
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: flash silme onaylandi
      }
    }
  }

  if (!erase_done) {
    Print("[FW] HATA: Flash silme zaman asimi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    goto fw_update_cleanup;
  }

  Print("[FW] Flash silindi!\r\n");
  HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: flash silindi, paket gondermeye basla

  /* ===================================================================
   * ADIM 4: PAKET TRANSFERI (RESUME DESTEKLI)
   *
   * PC'den 148 byte'lik paket al:
   *   [IV:16][AES_Encrypted:128][CRC32:4] = 148 byte
   *
   * İlk resume_start_packet kadar paket ATLANIYOR:
   *   - PC'den paket okunur (UART alimi yapilir)
   *   - RF'e gonderilmez (alici zaten bu paketleri yazmis)
   *   - PC'ye ACK verilir (PC normal akisla devam eder)
   *
   * Sonraki paketler normal sekilde gonderiliyor:
   *   - 148 byte'i 4 RF DATA_CHUNK'a bol
   *   - Her chunk icin ACK alinmazsa RF_MAX_RETRIES kez dene
   *   - Tum chunk'lar basariliysa PC'ye ACK gonderilir
   * =================================================================== */
  if (resume_start_packet > 0) {
    Print("[FW] Resume: ilk paketler atlaniyor...\r\n");
  } else {
    Print("[FW] Paket transferi basliyor...\r\n");
  }

  uint32_t packets_sent = 0; // Toplam islenen paket sayisi (atlananlar dahil)

  while (packets_sent < total_fw_packets) {
    HAL_IWDG_Refresh(&hiwdg);

    /* PC'den bir sonraki firmware paketini bekle (8 sn) */
    HAL_StatusTypeDef uart_status =
        HAL_UART_Receive(&huart1, fw_packet_buf, FW_FULL_PACKET_SIZE, 8000);

    if (uart_status != HAL_OK) {
      Print("[FW] HATA: UART alma hatasi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* Resume: bu paket alici tarafindan zaten yazilmissa RF'e gonderme.
     * PC'den okundu, ACK ver, sonraki pakete gec. */
    if (packets_sent < resume_start_packet) {
      packets_sent++;
      HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: paket alindi (atlanacak)
      continue; // RF gondermeden devam
    }

    /* 148 byte'i 4 RF chunk'a bol ve guvenilir gonder */
    uint8_t all_chunks_ok = 1;

    for (uint8_t chunk = 0; chunk < RF_CHUNKS_PER_PACKET; chunk++) {
      uint8_t chunk_payload[RF_MAX_DATA];
      uint8_t data_offset = chunk * RF_CHUNK_DATA_SIZE; // Her chunk 48 byte ilerler
      uint8_t data_len;

      if (chunk < RF_CHUNKS_PER_PACKET - 1) {
        data_len = RF_CHUNK_DATA_SIZE; // Ilk 3 chunk tam 48 byte
      } else {
        /* Son chunk: kalan veri (148 - 3x48 = 4 byte) */
        data_len = FW_FULL_PACKET_SIZE - data_offset;
        if (data_len > RF_CHUNK_DATA_SIZE) {
          data_len = RF_CHUNK_DATA_SIZE;
        }
      }

      /* Chunk payload yapisi: [chunk_index:1][chunk_toplam:1][veri:data_len] */
      chunk_payload[0] = chunk;                // Hangi parca? (0, 1, 2, 3)
      chunk_payload[1] = RF_CHUNKS_PER_PACKET; // Toplam parca sayisi (4)
      memcpy(&chunk_payload[2], &fw_packet_buf[data_offset], data_len); // Asil veri

      uint16_t chunk_seq = rf_seq_counter++;

      /* Guvenilir gonder: ACK gelene kadar RF_MAX_RETRIES kez dene */
      if (!RF_SendChunkReliable(RF_CMD_DATA_CHUNK, chunk_seq, chunk_payload,
                                data_len + 2)) {
        Print("[FW] HATA: Chunk gonderilemedi!\r\n");
        all_chunks_ok = 0;
        break;
      }
    }

    if (!all_chunks_ok) {
      /* Herhangi bir chunk basarisiz — PC'ye NACK */
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    packets_sent++;
    HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: bu 148 byte teslim edildi

    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin); // Her pakette LED yanip sonsun

    /* Her 10 pakette bir UART'a ilerleme yaz (debug amacli) */
    if (packets_sent % 10 == 0) {
      Print("[FW] Paket: ");
      PrintHex((uint8_t)(packets_sent >> 8));
      PrintHex((uint8_t)(packets_sent & 0xFF));
      Print("\r\n");
    }
  }

  /* ===================================================================
   * ADIM 4.5: ED25519 DİJİTAL İMZA GÖNDER
   * PC'den 64-byte Ed25519 imzasini al (R[32] || S[32]).
   * 2 RF_CMD_SIG_CHUNK paketiyle aliciya ilet:
   *   chunk 0: [0x00][imza[0..31]]  = 33 byte
   *   chunk 1: [0x01][imza[32..63]] = 33 byte
   * Her chunk icin ACK bekle, sonra PC'ye tek ACK ver.
   * =================================================================== */
  Print("[FW] Ed25519 imza bekleniyor...\r\n");
  {
    uint8_t sig_buf[64];  /* Ed25519 imzasi: R[32] || S[32] */

    /* PC'den 64-byte Ed25519 imzasini al */
    if (HAL_UART_Receive(&huart1, sig_buf, 64, 15000) != HAL_OK) {
      Print("[FW] HATA: Imza alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      goto fw_update_cleanup;
    }

    /* 2 SIG_CHUNK: her biri [idx:1][data:32] = 33 byte */
    for (uint8_t idx = 0; idx < 2; idx++) {
      uint8_t chunk[33];
      chunk[0] = idx;
      memcpy(&chunk[1], &sig_buf[idx * 32], 32);

      uint8_t chunk_ok = 0;
      for (uint8_t retry = 0; retry < RF_MAX_RETRIES && !chunk_ok; retry++) {
        RF_SendPacket(RF_CMD_SIG_CHUNK, rf_seq_counter, chunk, 33);
        HAL_IWDG_Refresh(&hiwdg);
        if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
          if (rx_type == RF_CMD_ACK) chunk_ok = 1;
        }
      }
      rf_seq_counter++;

      if (!chunk_ok) {
        Print("[FW] HATA: Imza chunk gonderilemedi!\r\n");
        HAL_UART_Transmit(&huart1, &nack, 1, 100);
        goto fw_update_cleanup;
      }
    }

    /* Imza aliciya ulasti — PC'ye ACK ver */
    HAL_UART_Transmit(&huart1, &ack, 1, 100);
    Print("[FW] Imza gonderildi\r\n");
  }

  /* ===================================================================
   * ADIM 5: FINAL DOGRULAMA SONUCU
   * Alici tum veriyi flash'a yazdiktan sonra CRC-32 dogrulamasi yapar.
   * UPDATE_COMPLETE → basari | UPDATE_FAILED → hata kodu ile basarisiz
   * =================================================================== */
  Print("[FW] Final sonucu bekleniyor...\r\n");

  uint8_t final_received = 0;
  uint32_t final_start = HAL_GetTick();

  while (!final_received && (HAL_GetTick() - final_start) < 45000) { // 45 sn timeout
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_UPDATE_COMPLETE) {
        /* Guncelleme basarili */
        final_received = 1;
        Print("[FW] *** GUNCELLEME BASARILI! ***\r\n");
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: tebrik alindi
        HAL_UART_Transmit(&huart1, &ack, 1, 100);   // PC'ye: basari

        /* Her iki LED 5 kez yanip sonsun — gorsel basari gostergesi */
        for (int i = 0; i < 5; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
          HAL_Delay(200);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
          HAL_Delay(200);
          HAL_IWDG_Refresh(&hiwdg);
        }
      } else if (rx_type == RF_CMD_UPDATE_FAILED) {
        /* Guncelleme basarisiz — alici hata kodu gonderdi */
        final_received = 1;
        Print("[FW] HATA: Guncelleme basarisiz! Hata kodu: ");
        if (rx_pld_len > 0) {
          PrintHex(rx_pld[0]); // RF_ERR_xxx kodu (rf_bootloader.h'e bak)
        }
        Print("\r\n");
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: mesaj alindi
        HAL_UART_Transmit(&huart1, &nack, 1, 100);  // PC'ye: basarisiz

        /* LED0 10 kez hizli yanip sonsun — hata gostergesi */
        for (int i = 0; i < 10; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_Delay(100);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_Delay(100);
          HAL_IWDG_Refresh(&hiwdg);
        }
      }
    }
  }

  if (!final_received) {
    /* 45 saniye icinde sonuc gelmedi */
    Print("[FW] Uyari: Final sonucu alinamadi (timeout)\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
  }

fw_update_cleanup:
  /* Temizlik: LED'leri sondur, debug print'i tekrar ac */
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  SenderDebug_SetEnabled(1); // Normal mod debug print'ini tekrar etkinlestir

  Print("[FW] Firmware update modu sona erdi\r\n");
}

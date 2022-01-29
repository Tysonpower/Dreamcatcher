#include "SPI.h"
#include "lr1110.h"
#include "customize.h"
#include "settings.h"
#include "esp_task_wdt.h"
#include "libb64/cdecode.h"
#include "libb64/cencode.h"
#include "carousel.h"
#include "driver/gpio.h"

carousel data_carousel;				// File downloader for this stream
portMUX_TYPE sxMux;
extern bool sdCardPresent;
unsigned int filepacket, filepackets;
char filename[260] = "";
AsyncUDP udp;
int32_t offset;

// LR1110 struct
typedef struct lr1110_context_t {
    SPIClass* spi;
    uint8_t nreset;
    uint8_t busy;
    uint8_t dio1;
    uint8_t nss;
};

lr1110_context_t lrRadio;
lr1110_system_version_t version;
SPIClass* spi1110;

uint32_t Frequency;
int32_t _Offset = 0;                        //offset frequency for calibration purposes  
uint8_t Bandwidth;          //LoRa bandwidth
uint8_t SpreadingFactor;        //LoRa spreading factor
uint8_t CodeRate;            //LoRa coding rate

uint32_t packetsRX = 0;
uint32_t IRQStatus;

uint8_t RXPacketL;                               //stores length of packet received
int8_t  PacketRSSI;                              //stores RSSI of received packet
int8_t  PacketSNR;                               //stores signal to noise ratio (SNR) of received packet
extern xTaskHandle rxTaskHandle;
static uint16_t crc, header;
bool isFormatting;

//    bitrate variables
static uint8_t packetsCount;
static uint8_t lastPacket;
static uint8_t packets[100] = {0};
static uint8_t y = 0;

#define BLINK_PIN    GPIO_NUM_34

bool loraReady;                           // variable to display LoRa fault with led or on website

struct midi_data {
    uint8_t note;
    float   start;
    uint8_t velocity;
    float   duration;
};

struct midi_data midiarray[32];
uint8_t firstnote = 0;
char* txtarray;

void init_gpio(void) {
  gpio_reset_pin(BLINK_PIN);
  gpio_set_direction(BLINK_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(BLINK_PIN, 0);
}

void blinky(void *pvParameter)
{
  gpio_set_level(BLINK_PIN, 1);
  vTaskDelay(50 / portTICK_RATE_MS); // sleep 500ms
  gpio_set_level(BLINK_PIN, 0);
  vTaskDelete(NULL);
}

/**
 * Helper function to feed website with stats
 */
extern "C" void getStats(uint16_t* _crc, uint16_t* _header)
{
  *_crc = crc;
  *_header = header;
}

extern "C" void getMidi(char* _txtarray)
{
  const char* txt_pre = "{\"type\":\"midi\",\"data\":[";
  const char* txt_suf = "]}";
  const char* txt_template = "[%d,%f,%d,%f],";
  int strlength = 0;
  strlength += sprintf(_txtarray+strlength, txt_pre);
  for (midi_data x : midiarray)
  {
    strlength += sprintf(_txtarray+strlength, txt_template, x.note, x.start, x.velocity, x.duration);
  }
  strlength += sprintf(_txtarray+strlength-1, txt_suf);
}

/**
 * Helper function to feed website with stats
 */
extern "C" void getPacketStats(int8_t* rssi, int8_t* snr, int8_t* ssnr)
{
  *rssi = PacketRSSI;
  *snr = PacketSNR;
  *ssnr = 0;
}

class mycallback : public carousel::callback {
  void fileComplete( const std::string &path ) {
    // Serial.printf("new file path: %s\n", path.c_str());
    strcpy(filename, path.c_str());
  }
	void processFile(unsigned int index, unsigned int count) {
    // Serial.printf("file progress: %d of %d packets\n", index, count);
    filepacket = index;
    filepackets = count;
  }
};

/**
 * ISR function from LR1110
 */
IRAM_ATTR void rx1110ISR()
{
  Serial.println("DIO1");
  xTaskNotify(rxTaskHandle, 0x0, eSetBits);
}

IRAM_ATTR void busyIRQ()
{
  //Serial.print("irq on busy, level: ");
  //Serial.println(digitalRead(33));
}

/**
 * Init LR1110 with default params, order is important:
 * 1) SetPacketType to LORA
 * 2) SetModulationParams
 * 3) SetPacketParams
 * 3a) Set radio frequency - optional here
 * 4) SetPAConfig
 * 5) SetTxParams
 * 6) Set dio1 irq
 */
void initLR1110()
{
  init_gpio();

  lr1110_system_stat1_t lrStat1;
  lr1110_system_stat2_t lrStat2;
  lr1110_system_irq_mask_t lrIrq_status; 

  pinMode(DIO1, INPUT);
  pinMode(RFBUSY, INPUT);
  pinMode(NRESET, OUTPUT);

  memset(&lrRadio, 0, sizeof(lr1110_context_t));
  lrRadio.busy = RFBUSY;
  lrRadio.dio1 = DIO1;
  lrRadio.nreset = NRESET;
  lrRadio.nss = LORA_NSS;

  //attachInterrupt(DIO1, rx1110ISR, RISING);

  spi1110 = new SPIClass(FSPI);
  spi1110->begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  lrRadio.spi = spi1110;

  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);

  //------------------------------------------------------

  lr1110_system_reset(&lrRadio);
  delay(220);
  lr1110_system_reboot(&lrRadio, false);

  delay(100);

  lr1110_system_set_reg_mode(&lrRadio, LR1110_SYSTEM_REG_MODE_DCDC);

  lr1110_system_get_status(&lrRadio, &lrStat1, &lrStat2, &lrIrq_status);
  Serial.print("DCDC Stat1: ");
  Serial.println(lrStat1.command_status);
  //lr1110_system_set_tcxo_mode(&lrRadio, LR1110_SYSTEM_TCXO_CTRL_3_0V, 0x70);
  lr1110_system_set_tcxo_mode(&lrRadio, LR1110_SYSTEM_TCXO_CTRL_1_8V, 0x70);
  
  lr1110_system_get_status(&lrRadio, &lrStat1, &lrStat2, &lrIrq_status);
  Serial.print("TCXO Stat1: ");
  Serial.println(lrStat1.command_status);

  delay(100);
  lr1110_system_cfg_lfclk(&lrRadio, LR1110_SYSTEM_LFCLK_RC, true);
  lr1110_system_clear_errors(&lrRadio);

  Serial.println("system calibrate");
  lr1110_system_calibrate(&lrRadio, 0x3F);

  lr1110_system_get_status(&lrRadio, &lrStat1, &lrStat2, &lrIrq_status);
  Serial.print("calibrate Stat1: ");
  Serial.println(lrStat1.command_status);
  //lr1110_system_calibrate(&lrRadio, LR1110_SYSTEM_CALIB_ADC_MASK);
  lr1110_system_clear_irq_status(&lrRadio, LR1110_SYSTEM_IRQ_ALL_MASK | 0x14 | 0x15);

  // SetPacketType (1)
  lr1110_radio_set_pkt_type(&lrRadio, LR1110_RADIO_PKT_TYPE_LORA);

  // SetModulationParams (2)
  lr1110_radio_mod_params_lora_t mod_params;
  //mod_params.sf = (lr1110_radio_lora_sf_t)SpreadingFactor;
  //mod_params.bw = (lr1110_radio_lora_bw_t)Bandwidth;
  //mod_params.cr = (lr1110_radio_lora_cr_t)CodeRate;

  mod_params.sf = (lr1110_radio_lora_sf_t)LR1110_RADIO_LORA_SF9;
  mod_params.bw = (lr1110_radio_lora_bw_t)LR1110_RADIO_LORA_BW_250;
  mod_params.cr = (lr1110_radio_lora_cr_t)LR1110_RADIO_LORA_CR_4_5;

  lr1110_radio_set_lora_mod_params(&lrRadio, &mod_params);

  // SetPacketParams (3)
  const lr1110_radio_pkt_params_lora_t pkt_params = {
      .preamble_len_in_symb = 20,                  //!< LoRa Preamble length [symbols]
      .header_type = LR1110_RADIO_LORA_PKT_IMPLICIT, //!< LoRa Header type configuration
      .pld_len_in_bytes = 255,                        //!< LoRa Payload length [bytes]
      .crc = LR1110_RADIO_LORA_CRC_ON,               //!< LoRa CRC configuration
      .iq = LR1110_RADIO_LORA_IQ_STANDARD,           //!< LoRa IQ configuration
  };
  lr1110_radio_set_lora_pkt_params(&lrRadio, &pkt_params);

  // Set radio freq (3a)
  lr1110_radio_set_rf_freq(&lrRadio, Frequency);
  //lr1110_radio_set_rf_freq(&lrRadio, 868200000);

  // SetPAConfig (4)
  const lr1110_radio_pa_cfg_t pa_cfg = {
      .pa_sel = LR1110_RADIO_PA_SEL_LP,                 //!< Power Amplifier selection
      .pa_reg_supply = LR1110_RADIO_PA_REG_SUPPLY_VBAT, //!< Power Amplifier regulator
      .pa_duty_cycle = 0x04,                            //!< Power Amplifier duty cycle (Default 0x04)
      .pa_hp_sel = 0x07                                 //!< Number of slices for HPA (Default 0x07)
  };
  lr1110_radio_set_pa_cfg(&lrRadio, &pa_cfg);

  // SetTxParams (5)
  lr1110_radio_set_tx_params(&lrRadio, 0, LR1110_RADIO_RAMP_48_US);

  // Set dio1 irq(6)
  lr1110_system_set_dio_irq_params(&lrRadio, LR1110_SYSTEM_IRQ_ALL_MASK | 0x14 | 0x15, 0);
  lr1110_system_clear_irq_status(&lrRadio, LR1110_SYSTEM_IRQ_ALL_MASK | 0x14 | 0x15);
  delay(100);

  attachInterrupt(DIO1, rx1110ISR, RISING);
  attachInterrupt(RFBUSY, busyIRQ, RISING);

  lr1110_radio_set_rx(&lrRadio, 0); //start Receiving
  //lr1110_radio_set_tx_infinite_preamble(&lrRadio);
  //lr1110_radio_set_tx_cw(&lrRadio);

  loraReady = true;

  lr1110_system_errors_t lrErrors;
  lr1110_system_get_errors( &lrRadio, &lrErrors );
  Serial.print("LR1110 Errors: ");
  Serial.println(lrErrors);
}

/**
 * Get LR1110 version info to confirm LR1110 is working
 */
void getLR1110Info()
{
  lr1110_system_get_version(&lrRadio, &version);
  Serial.printf("HW: 0x%02x, FW: 0x%04x\n", version.hw, version.fw);
  Serial.printf("Mode: 0x%02x\n", version.type);
  Serial.printf("\n");
}

extern "C" void updateLoraSettings(uint32_t freq, uint8_t bw, uint8_t sf, uint8_t cr)
{
  Frequency = freq;
  SpreadingFactor = sf;
  Bandwidth = bw;
  CodeRate = cr;
  lr1110_radio_mod_params_lora_t mod_params;
  //mod_params.sf = (lr1110_radio_lora_sf_t)SpreadingFactor;
  //mod_params.bw = (lr1110_radio_lora_bw_t)Bandwidth;
  //mod_params.cr = (lr1110_radio_lora_cr_t)CodeRate;

  mod_params.sf = (lr1110_radio_lora_sf_t)LR1110_RADIO_LORA_SF9;
  mod_params.bw = (lr1110_radio_lora_bw_t)LR1110_RADIO_LORA_BW_250;
  mod_params.cr = (lr1110_radio_lora_cr_t)LR1110_RADIO_LORA_CR_4_5;

  lr1110_radio_set_lora_mod_params(&lrRadio, &mod_params);
  lr1110_radio_set_rf_freq(&lrRadio, Frequency);
  lr1110_radio_set_rx(&lrRadio, 0); //start Receiving

  storeLoraSettings();
}

/**
 * Read data from LR1110
 */
uint8_t readbufferLR1110(uint8_t *rxbuffer, uint8_t size)
{
  uint8_t RXstart;
  uint8_t _RXPacketL = 0;
  uint32_t regdata;

  /*
  lr1110_system_get_and_clear_irq_status(&lrRadio, &regdata);

  if ( (regdata & LR1110_SYSTEM_IRQ_HEADER_ERROR) | (regdata & LR1110_SYSTEM_IRQ_CRC_ERROR) ) //check if any of the preceding IRQs is set
  {
    return 0;
  }
  */

  //get lor header info
  lr1110_radio_lora_cr_t pktCrInfo;
  bool CRCInfo;
  lr1110_radio_get_lora_rx_info(&lrRadio, &CRCInfo, &pktCrInfo);
  Serial.printf("Pkt header info - crc: %02X , codingrate: %02X \n", CRCInfo, pktCrInfo);

  //get lora stats
  lr1110_radio_stats_lora_t loraStats;
  lr1110_radio_get_lora_stats(&lrRadio, &loraStats);
  Serial.printf("Lorastats: RX %i, CRC %i, HEADER %i \n", loraStats.nb_pkt_received, loraStats.nb_pkt_crc_error, loraStats.nb_pkt_header_error);

  // get rx buffer size to read
  lr1110_radio_rx_buffer_status_t bufferStatus;
  lr1110_radio_get_rx_buffer_status(&lrRadio, &bufferStatus);

  _RXPacketL = bufferStatus.pld_len_in_bytes;
  RXstart = bufferStatus.buffer_start_pointer;

  // read rxbuffer over SPI afap
  uint8_t buffer[255] = {0};

  lr1110_regmem_read_buffer8(&lrRadio, buffer, RXstart, _RXPacketL);

  Serial.printf("RX Buffer (len: %i, offset: %i): \n", RXPacketL, RXstart);
  Serial.println((char*)buffer);

  return _RXPacketL;
}

/**
 * Read packet from LR1110, when IRQ is triggered
 */
void rxTaskLR1110(void* p)
{
  static uint32_t mask = 0;
  data_carousel.init("/files/tmp", new mycallback());
  while(1) {
    if (xTaskNotifyWait(0, 0, &mask, portMAX_DELAY))
    {
      Serial.println("rxTask triggered");

      lr1110_system_get_and_clear_irq_status(&lrRadio, &IRQStatus);
      //lr1110_system_clear_irq_status(&lrRadio, LR1110_SYSTEM_IRQ_ALL_MASK | 0x14 | 0x15);
      
      Serial.println(IRQStatus);
      if(IRQStatus == 0x0) continue;
      if(IRQStatus & LR1110_SYSTEM_IRQ_RX_DONE) {
        Serial.println("GOT A PACKET WOOHOO!");

        lr1110_radio_pkt_status_lora_t pkt_status;        
        lr1110_radio_get_lora_pkt_status( &lrRadio, &pkt_status );        

        PacketRSSI = pkt_status.rssi_pkt_in_dbm;              //read the recived RSSI value
        PacketSNR = pkt_status.snr_pkt_in_db;                //read the received SNR value
        //offset = LT.getFrequencyErrorRegValue();
        Serial.println("Packet stats------");
        Serial.printf("RSSI: %d, SNR: %d \n", PacketRSSI, PacketSNR);
        Serial.println("---------------------");

        uint8_t data[256];

        RXPacketL = readbufferLR1110(data, RXBUFFER_SIZE);

        xTaskCreate(&blinky, "blinky", 512,NULL,5,NULL);
        
        packetsRX++;
        lastPacket = y%100;
        packets[lastPacket] = RXPacketL;
        packetsCount++;
        y++;
        /*
        // Check if we got a special Realtime Packet
        // 0x73 = Midi Stream
        if(data[2] == 0x73){
          udp.writeTo(data+4, RXPacketL-8, IPAddress(239,1,2,3), 8281);
          uint8_t midibuf[10] = {0};
          int chunks = floor((RXPacketL-8)/10);
          //loop at data in 10byte chunks to parse Data
          memset(midiarray, 0, sizeof(midiarray));
          for (int i = 0; i < chunks; i++)
          {  
            memcpy(midibuf, data+4+i*10, 10);
          
            struct midi_data tmpmidi;

            memcpy(&tmpmidi.note, midibuf, 1);
            memcpy(&tmpmidi.start, midibuf+1, 4);
            memcpy(&tmpmidi.velocity, midibuf+5, 1);
            memcpy(&tmpmidi.duration, midibuf+6, 4);
            midiarray[i] = tmpmidi;
          }
        }
        */
        if(RXPacketL > 0) {
          if (!isFormatting) {            // stop consuming data during sd card formatting to not access card
            if (!sdCardPresent)
            {
              portENTER_CRITICAL(&sxMux);
                data_carousel.consume(data, RXPacketL);
              portEXIT_CRITICAL(&sxMux);
            } else {
              data_carousel.consume(data, RXPacketL);
            }
          }
          data[0] = RXPacketL;
          udp.writeTo(data, RXPacketL, IPAddress(239,1,2,3), 8280);
        }
        
      }

      if (IRQStatus & LR1110_SYSTEM_IRQ_CRC_ERROR) {
        //Serial.println("LR1110_SYSTEM_IRQ_CRC_ERROR");
        crc++;
      } 
      if (IRQStatus & LR1110_SYSTEM_IRQ_HEADER_ERROR) {
        //Serial.println("LR1110_SYSTEM_IRQ_HEADER_ERROR");
        header++;
      }
      /*
      if(IRQStatus & LR1110_SYSTEM_IRQ_PREAMBLE_DETECTED)
      {
        Serial.println("LR1110_SYSTEM_IRQ_PREAMBLE_DETECTED");
      }*/

      lr1110_radio_set_rx( &lrRadio, 0);
      Serial.println("Set Radio back to RX");
    }
  }
}

/**
 * Simple function to count LoRa bitrate
 * @param update - time in millis from last update
 */
uint16_t countBitrate(uint16_t update)
{
  uint16_t bitrate = 0;
  int n = 0;

  do
  {
    n = lastPacket - packetsCount;
    if (n < 0)
    {
      n += 100;
    }
    
    bitrate += packets[n];
    packetsCount--;
  } while (packetsCount > 0);

  log_d("bitrate: %d bits/s\n", bitrate * 8 * 1000/update);

  return bitrate * 8 * 1000/update; // bitrate in bits/s
}
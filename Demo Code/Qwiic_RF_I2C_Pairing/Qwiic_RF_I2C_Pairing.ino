#include <Wire.h> //Need this for I2C
#include <EEPROM.h> //Need this for EEPROM Read/Write
#include <SPI.h> //Need this to talk to the radio
#include <LoRa.h> //https://github.com/sandeepmistry/arduino-LoRa

//Location in EEPROM where various settings will be stored
#define LOCATION_I2C_ADDR 0x01
#define LOCATION_RADIO_ADDR 0x02
#define LOCATION_SYNC_WORD 0x03
#define LOCATION_SPREAD_FACTOR 0x04
#define LOCATION_MESSAGE_TIMEOUT 0x05
#define LOCATION_TX_POWER 0x06
#define LOCATION_PAIRED_ADDRESS 0x07

//There is an ADR jumpber on this board. When closed, forces I2C address to default.
#define I2C_ADDRESS_DEFAULT 0x35
#define I2C_ADDRESS_JUMPER_CLOSED 0x36

//These are the commands we understand and may respond to
#define COMMAND_GET_STATUS 0x01
#define COMMAND_SEND 0x02
#define COMMAND_SEND_RELIABLE 0x03
#define COMMAND_SET_RELIABLE_TIMEOUT 0x04
#define COMMAND_GET_PAYLOAD 0x05
#define COMMAND_SET_SPREAD_FACTOR 0x06
#define COMMAND_SET_SYNC_WORD 0x07
#define COMMAND_GET_SYNC_WORD 0x11
#define COMMAND_SET_RF_ADDRESS 0x08
#define COMMAND_GET_RF_ADDRESS 0x09
#define COMMAND_GET_PACKET_RSSI 0x0A
#define COMMAND_GET_PAYLOAD_SIZE 0x0B
#define COMMAND_GET_PACKET_SENDER 0x0C
#define COMMAND_GET_PACKET_RECIPIENT 0x0D
#define COMMAND_GET_PACKET_SNR 0x0E
#define COMMAND_GET_PACKET_ID 0x0F
#define COMMAND_SET_TX_POWER 0x10
#define COMMAND_SET_I2C_ADDRESS 0x20
#define COMMAND_SET_PAIRED_ADDRESS 0x12
#define COMMAND_GET_PAIRED_ADDRESS 0x13
#define COMMAND_GET_SKU 0x15
#define COMMAND_SEND_PAIRED 0x20
#define COMMAND_SEND_RELIABLE_PAIRED 0x30

//These will help us keep track of how to respond to requests
#define RESPONSE_TYPE_STATUS 0x00
#define RESPONSE_TYPE_PAYLOAD 0x01
#define RESPONSE_TYPE_RF_ADDRESS 0x02
#define RESPONSE_TYPE_PACKET_RSSI 0x03
#define RESPONSE_TYPE_PACKET_SIZE 0x04
#define RESPONSE_TYPE_PACKET_SENDER 0x05
#define RESPONSE_TYPE_PACKET_RECIPIENT 0x06
#define RESPONSE_TYPE_PACKET_SNR 0x07
#define RESPONSE_TYPE_PACKET_ID 0x08
#define RESPONSE_TYPE_SYNC_WORD 0x09
#define RESPONSE_TYPE_PAIRED_ADDRESS 0x10
#define RESPONSE_TYPE_SKU 0x11

//A Few Pin Definitions
#define ADR_JUMPER 4
#define PAIR_BTN A1
#define PAIR_LED A2
#define PWR_LED A3

//Firmware version. This is sent when requested. Helpful for tech support.
const byte firmwareVersionMajor = 0;
const byte firmwareVersionMinor = 5;
char SKU[] = "14788";

//RFM95 pins
const int csPin = 10;          // LoRa radio chip select
const int resetPin = 9;       // LoRa radio reset
const int irqPin = 3;         // change for your board; must be a hardware interrupt pin

//*** These get twiddled during interrupts so they're type volatile ***

//System status variable
//Bit 0 - Ready To Send
//Bit 1 - Packet Available
//Bit 2 - Waiting on Reliable Send
//Bit 3 - Reliable Send Timeout
volatile byte systemStatus = 0b00000000;
//This global is used to keep track of the I2C Address (Software Switchable)
volatile byte settingI2CAddress = I2C_ADDRESS_DEFAULT;
//These globals are used for the "Reliable Send" routine
volatile unsigned long reliableSendTime = 0x00;
volatile unsigned long reliableResend = 0x00;
volatile byte reliableSendChk = 0x00;

//*** ***

//These are the radio parameters that are used to
//Initialize the RFM95. We assign them default
//values so we can load EEPROM on first boot.
byte settingRFAddress = 0xBB;
byte settingSyncWord = 0x34;
byte settingSpreadFactor = 0x07;
byte settingMessageTimeout = 0x0A;
byte settingTXPower = 0x11;
byte settingPairedAddress = 0xBB;
byte msgCount = 0x00;

//This is a type for storing radio packets
typedef struct {
  byte id;
  byte sender;
  byte recipient;
  byte snr;
  byte rssi;
  byte reliable;
  byte payloadLength;
  String payload;
} packet;

//Let's initialize a few instances of our new type
packet lastReceived = {0, 0, 0, 0, 0, 0, 0, ""};
packet lastSent = {0, 0, 0, 0, 0, 0, 0, ""};
packet outbox = {0, 0, 0, 0, 0, 0, 0, ""};

//Counter for the Pairing Button Hold-down
uint16_t pair_hold = 0;

//This will help us keep track of how to respond to requests
byte responseType = RESPONSE_TYPE_STATUS;

//This flag will tell the main loop when an outgoing packet is waiting for the radio
boolean outbox_waiting = 0;

//This flag will tell the main loop to mark the beginning of a reliable send cycle
boolean mark_time_reliable = 0;

void setup()
{
    
  //Set pin modes
  pinMode(PAIR_BTN, INPUT_PULLUP);
  pinMode(ADR_JUMPER, INPUT_PULLUP);
  pinMode(PAIR_LED, OUTPUT);
  pinMode(PWR_LED, OUTPUT);

  readSystemSettings(); //Load all system settings from EEPROM

  // override the default CS, reset, and IRQ pins (optional)
  LoRa.setPins(csPin, resetPin, irqPin);// set CS, reset, IRQ pin

  if (!LoRa.begin(915E6)) { //Initialize ratio at 915 MHz
    while (true) {
      //If failed, blink power LED
      digitalWrite(PWR_LED, 1);
      delay(500);
      digitalWrite(PWR_LED, 0);
      delay(500);
    };
  } else {
    //If successful, light power LED
    digitalWrite(PWR_LED, 1);
  }

  //Set our radio parameters from the stored values
  LoRa.setSyncWord(settingSyncWord);
  LoRa.setSpreadingFactor(settingSpreadFactor);
  LoRa.setTxPower(settingTXPower);

  LoRa.enableCrc();

  //Begin listening on I2C only after we've setup all our config and opened any files
  startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
  systemStatus |= 1 << 0; //Set "Ready" Status Flag
}

void loop()
{

  //If we just started a reliable send cycle, we need to mark the start time.
  //We do it here instead of immediately after getting the command so that we aren't
  //loading an unsigned long during an ISR
  if (mark_time_reliable) {
    systemStatus |= 1 << 2; //Set "Waiting on Reliable Send" Status Flag
    reliableSendTime = reliableResend = millis();
    mark_time_reliable = 0;
  }

  //If there is a "Waiting on Reliable Send" flag and the ack timer has expired,
  //remove the flag and set the "Reliable Send Timeout" flag instead.
  if ( ( (systemStatus >> 2) & 1 ) && ( millis() > ( reliableSendTime + ( settingMessageTimeout * 1000 ) ) ) ) {
    systemStatus &= ~(1 << 2); //Clear "Waiting on Reliable Send" Status Flag
    systemStatus |= 1 << 0; //Set "Ready" Status Flag
    systemStatus |= 1 << 3; //Set "Reliable Send Timeout" Status Flag
    reliableSendChk = 0x00; //Reset Checksum accumulator
    reliableSendTime = 0x00; //Reset reliableSendTime timestamp
    reliableResend = 0x00; //Reset reliableResend timestamp
    //If there is a "Waiting on Reliable Send" flag and the ack timer has NOT expired,
    //check the reliable send interval counter and possibly try resending.
  } else if ( ( (systemStatus >> 2) & 1 ) && ( millis() < ( reliableSendTime + ( settingMessageTimeout * 1000 ) ) ) ) {
    if ( millis() > reliableResend + 1000 ) { //Retry once per second
      sendMessage(lastSent.recipient, 0, lastSent.payload);
      reliableResend = millis();
    }
  }

  //If the pairing button is being held
  if ( digitalRead(PAIR_BTN) == 0 ) {
    pair_hold++;
    if (pair_hold > 5000) {
      pair_hold = 0;
      pairingSequence();
    }
  } else {
    pair_hold = 0;
  }

  if (outbox_waiting) {
    sendMessage(outbox.recipient, outbox.reliable, outbox.payload);
    outbox_waiting = 0;
    systemStatus |= 1 << 0; //Set "Ready" Status Flag
  }

  onReceive(LoRa.parsePacket());
}

//Send a message via the radio
void sendMessage(byte destination, byte reliable, String outgoing)
{
  digitalWrite(PAIR_LED, 1);
  LoRa.beginPacket();                   // start packet
  LoRa.write(destination);              // add destination address
  LoRa.write(settingRFAddress);         // add sender address
  LoRa.write(msgCount);                 // add message ID
  LoRa.write(reliable);                 // add reliable send tag
  LoRa.write(outgoing.length());        // add payload length
  LoRa.print(outgoing);                 // add payload
  LoRa.endPacket();                     // finish packet and send it

  lastSent.id = msgCount;
  lastSent.sender = settingRFAddress;
  lastSent.recipient = destination;
  lastSent.payloadLength = outgoing.length();
  lastSent.payload = outgoing;
  lastSent.reliable = reliable;

  msgCount++;                           // increment message ID
  digitalWrite(PAIR_LED, 0);
}

//Check for a new radio packet, parse it, store it
void onReceive(int packetSize) {
  if (packetSize == 0) return;          // if there's no packet, return

  digitalWrite(PAIR_LED, 1);
  // read packet header bytes:
  int recipient = LoRa.read();          // recipient address
  byte sender = LoRa.read();            // sender address
  byte incomingMsgId = LoRa.read();     // incoming msg ID
  byte reliable = LoRa.read();          // reliable send tag
  byte incomingLength = LoRa.read();    // incoming msg length

  String incoming = "";

  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  digitalWrite(PAIR_LED, 0);

  if (incomingLength != incoming.length()) {   // check length for error
    return; //Get outta here
  }

  //If the packet isn't addressed to this device or the broadcast channel,
  if (recipient != settingRFAddress && recipient != 0xFF) {
    return; //Get outta here
  }

  //If the message is for this device, or broadcast, store it appropriately:
  lastReceived.id = incomingMsgId;
  lastReceived.sender = sender;
  lastReceived.recipient = recipient;
  lastReceived.snr = LoRa.packetSnr();
  lastReceived.rssi = LoRa.packetRssi();
  lastReceived.payloadLength = incomingLength;
  lastReceived.payload = incoming;
  lastReceived.reliable = reliable;

  //If we're waiting on a Reliable Send ack, check to see if this is it.
  //A Reliable Send Ack payload has two bytes:
  //Byte 1: SNR of Reliable Packet
  //Byte 2: RSSI of Reliable Packet
  if ( (systemStatus >> 2) & 1 ) {

    //If the fourth byte of the header is equal to the sum of
    //the last sent message, we have Reliable Ack.
    if ( reliable == reliableSendChk ) {

      reliableSendChk = 0x00; //Reset Checksum accumulator
      reliableSendTime = 0x00; //Reset reliableSendTime timestamp
      reliableResend = 0x00; //Reset reliableResend timestamp
      systemStatus &= ~(1 << 2); //Clear "Waiting on Reliable Send" Status Flag
      systemStatus |= 1 << 0; //Set "Ready" Status Flag
      systemStatus &= ~(1 << 3); //Clear "Reliable Send Timeout" Status Flag

      //Grab these tidbits in case anyone wants them
      lastSent.snr = incoming.charAt(0);
      lastSent.rssi = incoming.charAt(1);

    }
  }

  //If this was a Reliable type message, calculate checksum and reply
  if ( reliable == 1 ) {

    String response = "";
    byte reliableAckChk = 0;
    //Calculate simple checksum of payload, reliableSendChk is type byte
    //so for sake of cycles, we will let it roll instead of explicitly
    //calculating (sum payload % 256)
    for ( int symbol = 0; symbol < incomingLength; symbol++ ) {
      reliableAckChk += incoming.charAt(symbol);
    }
    //Because we use values 0 and 1 for signalling, we ensure that the
    //checksum can never be < 1
    if ( reliableAckChk < 254 ) {
      reliableAckChk += 2;
    }
    response = lastReceived.snr;
    response += lastReceived.rssi;
    //Return to Sender
    sendMessage(sender, reliableAckChk, response);

  }

  //Set the "New Payload" Status Flag
  systemStatus |= 1 << 1;
}

//Read an I2C Request from the master
void receiveEvent(int numberOfBytesReceived)
{
  //Record bytes to local array
  byte incoming = Wire.read();

  //Set new I2C address
  if (incoming == COMMAND_SET_I2C_ADDRESS)
  {
    if (Wire.available())
    {
      settingI2CAddress = Wire.read();

      //Error check
      if (settingI2CAddress < 0x08 || settingI2CAddress > 0x77)
        return; //Command failed. This address is out of bounds.

      EEPROM.write(LOCATION_I2C_ADDR, settingI2CAddress);

      //Our I2C address may have changed because of user's command
      startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
    }
  }
  //Return the current system status
  else if (incoming == COMMAND_GET_STATUS)
  {
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Send a payload via the radio
  else if (incoming == COMMAND_SEND)
  {

    if (Wire.available() > 1) {

      byte recipient = Wire.read();

      String payload = "";

      while (Wire.available()) {
        payload += (char)Wire.read();
      }

      queueMessage(recipient, 0, payload);

    }

  }
  //Send a payload via the radio to the last
  //paired address
  else if (incoming == COMMAND_SEND_PAIRED)
  {

    if (Wire.available() > 1) {

      String payload = "";

      while (Wire.available()) {
        payload += (char)Wire.read();
      }

      queueMessage(settingPairedAddress, 0, payload);

    }

  }  
  //Send a payload via the radio and request ack
  else if (incoming == COMMAND_SEND_RELIABLE)
  {

    if (Wire.available() > 1) {

      byte recipient = Wire.read();

      String payload = "";

      while (Wire.available()) {
        payload += (char)Wire.read();
      }

      queueMessage(recipient, 1, payload);

      //Reset checksum accumulator
      reliableSendChk = 0;

      //Calculate simple checksum of payload, reliableSendChk is type byte
      //so for sake of cycles, we will let it roll instead of explicitly
      //calculating (sum payload % 256)
      for ( int symbol = 0; symbol < payload.length(); symbol++ ) {
        reliableSendChk += payload.charAt(symbol);
      }

      //Because we use values 0 and 1 for signalling, we ensure that the
      //checksum can never be < 1
      if ( reliableSendChk < 254 ) {
        reliableSendChk += 2;
      }

      mark_time_reliable = 1;

    }

  }
  //Send a payload via the radio to the last paired
  //address and request ack
  else if (incoming == COMMAND_SEND_RELIABLE_PAIRED)
  {

    if (Wire.available() > 1) {

      String payload = "";

      while (Wire.available()) {
        payload += (char)Wire.read();
      }

      queueMessage(settingPairedAddress, 1, payload);

      //Reset checksum accumulator
      reliableSendChk = 0;

      //Calculate simple checksum of payload, reliableSendChk is type byte
      //so for sake of cycles, we will let it roll instead of explicitly
      //calculating (sum payload % 256)
      for ( int symbol = 0; symbol < payload.length(); symbol++ ) {
        reliableSendChk += payload.charAt(symbol);
      }

      //Because we use values 0 and 1 for signalling, we ensure that the
      //checksum can never be < 1
      if ( reliableSendChk < 254 ) {
        reliableSendChk += 2;
      }

      mark_time_reliable = 1;

    }

  }  
  //Set the time in seconds to wait for reliable ack before failing
  else if (incoming == COMMAND_SET_RELIABLE_TIMEOUT)
  {

    if (Wire.available()) {
      settingMessageTimeout = Wire.read();
      EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);
    }

  }
  //Store the address of a paired radio
  else if (incoming == COMMAND_SET_PAIRED_ADDRESS)
  {

    if (Wire.available()) {
      settingPairedAddress = Wire.read();
      EEPROM.write(LOCATION_PAIRED_ADDRESS, settingPairedAddress);
    }

  }
  //Return the address of a paired radio
  else if (incoming == COMMAND_GET_PAIRED_ADDRESS)
  {

    responseType = RESPONSE_TYPE_PAIRED_ADDRESS;

  }  
  //Return the payload of the last received packet
  else if (incoming == COMMAND_GET_PAYLOAD)
  {

    responseType = RESPONSE_TYPE_PAYLOAD;

  }
  //Set the spread factor of the radio
  else if (incoming == COMMAND_SET_SPREAD_FACTOR)
  {

    if (Wire.available()) {
      byte newSpreadFactor = Wire.read();

      if (newSpreadFactor > 12 || newSpreadFactor < 6) {
        return;
      }

      settingSpreadFactor = newSpreadFactor;
      EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);
      LoRa.setSpreadingFactor(settingSpreadFactor);
    }

  }
  //Set the Sync Word of the radio
  else if (incoming == COMMAND_SET_SYNC_WORD)
  {
    if (Wire.available()) {
      settingSyncWord = Wire.read();
      EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
      LoRa.setSyncWord(settingSyncWord);
    }

  }
  //Set the Address of the radio
  else if (incoming == COMMAND_SET_RF_ADDRESS)
  {

    if (Wire.available()) {
      byte newRFAddress = Wire.read();

      if (newRFAddress < 0xFF) { //0xFF is Broadcast Channel
        settingRFAddress = newRFAddress;
        EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
      }
    }

  }
  //Return the Address of the radio
  else if (incoming == COMMAND_GET_RF_ADDRESS)
  {

    responseType = RESPONSE_TYPE_RF_ADDRESS;
    return;

  }
  //Return the RSSI of the last received packet
  else if (incoming == COMMAND_GET_PACKET_RSSI)
  {

    responseType = RESPONSE_TYPE_PACKET_RSSI;
    return;

  }
  //Return the size of the payload of the last received packet
  else if (incoming == COMMAND_GET_PAYLOAD_SIZE)
  {

    responseType = RESPONSE_TYPE_PACKET_SIZE;
    return;

  }
  //Return the origin address of the last received packet
  else if (incoming == COMMAND_GET_PACKET_SENDER)
  {

    responseType = RESPONSE_TYPE_PACKET_SENDER;
    return;

  }
  //Return the destination address of the last received packet
  else if (incoming == COMMAND_GET_PACKET_RECIPIENT)
  {

    responseType = RESPONSE_TYPE_PACKET_RECIPIENT;
    return;

  }
    else if (incoming == COMMAND_GET_SYNC_WORD)
  {

    responseType = RESPONSE_TYPE_SYNC_WORD;
    return;

  }
  //Return the SNR (Signal-to-Noise Ratio) of the last received packet
  else if (incoming == COMMAND_GET_PACKET_SNR)
  {

    responseType = RESPONSE_TYPE_PACKET_SNR;
    return;

  }
  //Return the sequential ID of the last received packet
  else if (incoming == COMMAND_GET_PACKET_ID)
  {

    responseType = RESPONSE_TYPE_PACKET_ID;
    return;

  }
  //Return the SKU of this product. Used to identify
  //other radios on the bus during pairing operations.
  else if (incoming == COMMAND_GET_SKU)
  {

    responseType = RESPONSE_TYPE_SKU;
    return;

  }
  //Adjust the radio transmit amplifier
  else if (incoming == COMMAND_SET_TX_POWER)
  {

    byte txPower = Wire.read();
    if (txPower > 17) {
      txPower = 17;
    }
    settingTXPower = txPower;
    EEPROM.write(LOCATION_TX_POWER, settingTXPower);
    LoRa.setTxPower(txPower);

  }
}

//Respond to I2C master's request for bytes
void requestEvent()
{
  //Return system status byte
  if (responseType == RESPONSE_TYPE_STATUS)
  {
    Wire.write(systemStatus);
  }
  //Return payload of last received packet
  else if (responseType == RESPONSE_TYPE_PAYLOAD)
  {

    for (byte len = 0; len < lastReceived.payloadLength; len++) {
      Wire.write(lastReceived.payload.charAt(len));
    }

    responseType = RESPONSE_TYPE_STATUS;
    systemStatus &= ~(1 << 1); //Clear "New Payload" Status Flag
  }
  //Return current local address
  else if (responseType == RESPONSE_TYPE_RF_ADDRESS)
  {
    Wire.write(settingRFAddress);
    responseType = RESPONSE_TYPE_STATUS;
  }  
  else if (responseType == RESPONSE_TYPE_SYNC_WORD)
  {
    Wire.write(settingSyncWord);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the RSSI of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_RSSI)
  {
    Wire.write(lastReceived.rssi);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the size of the payload in the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SIZE)
  {
    Wire.write(lastReceived.payloadLength);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the origin address of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SENDER)
  {
    Wire.write(lastReceived.sender);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the destination address of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_RECIPIENT)
  {
    Wire.write(lastReceived.recipient);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the SNR (Signal-to-Noise Ratio) of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SNR)
  {
    Wire.write(lastReceived.snr);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the sequential ID of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_ID)
  {
    Wire.write(lastReceived.id);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PAIRED_ADDRESS)
  {
    Wire.write(settingPairedAddress);
    responseType = RESPONSE_TYPE_STATUS;
  }  
  //This is used to identify other radios on the bus during
  //pairing operations.
  else if (responseType == RESPONSE_TYPE_SKU)
  {
    Wire.write(SKU, 5);
    responseType = RESPONSE_TYPE_STATUS;
  }  
  else //By default we respond with the system status    
  {
    Wire.write(systemStatus);
  }
}

//Restore radio parameters or defaults
void readSystemSettings(void)
{

  //If this is the first ever boot, or EEPROM was nuked, load defaults to EEPROM:
  if ( EEPROM.read(LOCATION_RADIO_ADDR) == 0xFF ) {

    EEPROM.write(LOCATION_I2C_ADDR, I2C_ADDRESS_DEFAULT);
    EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);
    EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);
    EEPROM.write(LOCATION_TX_POWER, settingTXPower);
    EEPROM.write(LOCATION_PAIRED_ADDRESS, settingPairedAddress);
    
    //If not, load radio paramters from EEPROM
  } else {

    settingRFAddress = EEPROM.read(LOCATION_RADIO_ADDR);
    settingSyncWord = EEPROM.read(LOCATION_SYNC_WORD);
    settingSpreadFactor = EEPROM.read(LOCATION_SPREAD_FACTOR);
    settingMessageTimeout = EEPROM.read(LOCATION_MESSAGE_TIMEOUT);
    settingTXPower = EEPROM.read(LOCATION_TX_POWER);
    settingPairedAddress = EEPROM.read(LOCATION_PAIRED_ADDRESS);

  }
}

//Begin listening on I2C bus as I2C slave using the global variable setting_i2c_address
void startI2C()
{
  Wire.end(); //Before we can change addresses we need to stop

  if (digitalRead(ADR_JUMPER) == HIGH) //Default is HIGH.
    Wire.begin(settingI2CAddress); //Start I2C and answer calls using address from EEPROM
  else //User has closed jumper with solder to GND
    Wire.begin(I2C_ADDRESS_JUMPER_CLOSED); //Force address to I2C_ADDRESS_NO_JUMPER if user has opened the solder jumper

  //The connections to the interrupts are severed when a Wire.begin occurs. So re-declare them.
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}

//To avoid gross clock stretching and general strangeness, I moved radio 
//operations out of the I2C ISR. This function passes stuff out of the 
//ISR and sets flags for the radio routine to pick up later.
void queueMessage(byte addr, byte reliable, String payload) {

  systemStatus &= ~(1 << 0); //Clear "Ready" Status Flag

  //Place packet info in the global outbox
  outbox.recipient = addr;
  outbox.reliable = reliable;
  outbox.payload = payload;
  
  outbox_waiting = 1; //Tell the handler in the main loop that the outbox is ready

}

//Pairing Sequence:
//Pairing results in 
void pairingSequence(void)
{
    //Turn on the Status LED to alert the user that pairing has begun
    digitalWrite(PAIR_LED, 1);

    //Leave the I2C bus and then come back as a master
    Wire.end();
    Wire.begin();
    
    //Generate some random addresses and a new sync word
    randomSeed(LoRa.random()); //Seed PRNG with wideband RSSI
    byte newSyncWord = random(0x01, 0xFF); //Randomly generate a new SyncWord
    byte newRFAddressA = random(0x01, 0xF0); //Randomly generate a new RF Address for Initiator
    byte newRFAddressB = random(0x01, 0xF0); //Randomly generate a new RF Address for Subscriber
    byte sub_addr = 0x00;
    String incoming = "";
    
    //Try to avoid collisions
    while( newRFAddressA == newRFAddressB ) {
      newRFAddressB = random(0x01, 0xF0); //Randomly generate a new RF Address for Subscriber
    }

    //Find the Subscriber's I2C Address
    for ( byte addr = 0x08 ; addr < 0x7F ; addr++ ) {

      Wire.beginTransmission(addr);
      Wire.write(COMMAND_GET_SKU);
      Wire.endTransmission(false);
      Wire.requestFrom(addr, 5);
      
      while ( Wire.available() ) {
        incoming += char( Wire.read() );
      }

      if ( incoming == "14788" ) {
        sub_addr = addr;
        break;
      }else{
        incoming = "";
      }

    }

    // Set the Subscriber's Sync Word 
    while ( QwiicRF_GetSyncWord(sub_addr) != newSyncWord ) {

      QwiicRF_SetSyncWord(sub_addr, newSyncWord);
      delay(200);
      
    }

    //Set the Subscriber's RF Address
    while ( QwiicRF_GetRFAddress(sub_addr) != newRFAddressB ) {

      QwiicRF_SetRFAddress(sub_addr, newRFAddressB);
      delay(200);
      
    }

    //Set the Subscriber's Paired RF Address to our own
    while ( QwiicRF_GetPairedAddress(sub_addr) != newRFAddressA ) {

      QwiicRF_SetPairedAddress(sub_addr, newRFAddressA);
      delay(200);
      
    }

    //Now that the Subscriber is configured, let's config ourselves
    settingSyncWord = newSyncWord;
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    LoRa.setSyncWord(settingSyncWord);
    
    settingRFAddress = newRFAddressA;
    EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);

    settingPairedAddress = newRFAddressB;
    EEPROM.write(LOCATION_PAIRED_ADDRESS, settingPairedAddress);

    //Turn off the Status LED to alert the user that pairing has finished
    digitalWrite(PAIR_LED, 0);    

    //Return to I2C Slave Mode
    startI2C();

}

/************************************************************************
 * I2C MASTER FUNCTIONS FOR PAIRING
 */

byte QwiicRF_GetSyncWord(byte i2c_addr){
    byte qrfSync = 0x00;
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_GET_SYNC_WORD); 
  Wire.endTransmission(false);
  Wire.requestFrom(i2c_addr, 1);
  while (Wire.available()) {
    qrfSync = Wire.read();
  }

  return qrfSync;
}

void QwiicRF_SetSyncWord(byte i2c_addr, byte syncword)
{
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_SET_SYNC_WORD); 
  Wire.write(syncword); // Recipient: i2c_addr
  Wire.endTransmission(); 
}

byte QwiicRF_GetRFAddress(byte i2c_addr){
    byte qrfAddr = 0x00;
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_GET_RF_ADDRESS);
  Wire.endTransmission(false);
  Wire.requestFrom(i2c_addr, 1);
  while (Wire.available()) {
    qrfAddr = Wire.read();
  }

  return qrfAddr;
}

void QwiicRF_SetRFAddress(byte i2c_addr, byte addr)
{
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_SET_RF_ADDRESS);
  Wire.write(addr); // Recipient: i2c_addr
  Wire.endTransmission(); 
}

byte QwiicRF_GetPairedAddress(byte i2c_addr){
    byte qrfAddr = 0x00;
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_GET_PAIRED_ADDRESS);
  Wire.endTransmission(false);
  Wire.requestFrom(i2c_addr, 1);
  while (Wire.available()) {
    qrfAddr = Wire.read();
  }

  return qrfAddr;
}

void QwiicRF_SetPairedAddress(byte i2c_addr, byte addr)
{
  Wire.beginTransmission(i2c_addr);
  Wire.write(COMMAND_SET_PAIRED_ADDRESS); // Command: Set Paired Address
  Wire.write(addr); // Recipient: i2c_addr
  Wire.endTransmission(); 
}




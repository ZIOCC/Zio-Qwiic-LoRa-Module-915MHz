# Zio Qwiic LoRa Module 915MHz
We referred sparkfun's gateway firmware, check SparkFun QwiicRF Library master [here](https://github.com/sparkfun/SparkFun_QwiicRF_Library)

> This module is available for purchase [here](https://www.smart-prototyping.com/Zio-Qwiic-Lora-Module-915MHz).

![Zio Qwiic Lora Module 915](/lora915.jpg)

###### Description

This is a US compatible LoRa module (SX1276, 915MHz). LoRa module can send and receive data in a very long distance but consume very low power. We made a Qwiic version of this LoRa module, no soldering and jumper wire connections, just Qwiic cable plugin, and have fun! 

LoRa module is powerful, it can send and receive data packets at up to 10KM distance, and consumes very low power (10mA for receiver, 120mA for transmitter).

LoRa itself is a SPI device, in order to turn it into a I2C device, we added a MCU (ATMEGA328P) as the gateway to do the dirty work for you, you just need to use the module’s I2C interface to communicate with the others. It is just that easy.
 
The button on the board is for pairing function, check [here](https://github.com/sparkfun/SparkFun_QwiicRF_Library/wiki) for more details.

> Note: This LoRa module is based on SX1276 RFM95W-915S2, the frequency is 915 MHz, check [here](https://www.thethingsnetwork.org/docs/lorawan/frequencies-by-country.html) to see if this can be used in your country.


###### Specification

* Voltage: 3.3V
* Interface: I2C
* LoRa IC: SX1276 RFM95W-915S2
* Distance: Up to 10KM
* Gateway MCU: ATMEGA328P
* Dimension: 26.0 x 41.6 mm
* Weight: 5.3g


###### Links

* [PCB Source file](https://github.com/ZIOCC/Zio-Qwiic-LoRa-Module-915MHz/tree/master/EAGLE)
* [PCB schematic](https://github.com/ZIOCC/Zio-Qwiic-LoRa-Module-915MHz/blob/master/Zio%20Qwiic%20LoRa%20module%20915mhz.pdf)
* [Demo Code](https://github.com/ZIOCC/Zio-Qwiic-LoRa-Module-915MHz/tree/master/Demo%20Code/Qwiic_RF_I2C_Pairing)
* [Gateway firmware](https://github.com/sparkfun/SparkFun_QwiicRF_Library)



###### About Zio
> Zio is a new line of open sourced, compact, and grid layout boards, fully integrated for Arduino and Qwiic ecosystem. Designed ideally for wearables, robotics, small-space limitations or other on the go projects. Check out other awesome Zio products [here](https://www.smart-prototyping.com/Zio).

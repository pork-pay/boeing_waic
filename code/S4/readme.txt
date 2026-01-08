ble_A (esp32&BMP180): 
	Function: Transmit the tempurature and RSSI to the ble_B_b_test by BLE communication;
	Physical Connection: esp32 connects to the BMP180 (VIN: 3.3；GND: GND; SCL: GPIO 5; SDA: GPIO 4;)

ble_B (esp32&esp8266): 
	ble_B_b_test(esp32)
		Function: Receive the tempurature, control the fan and send the data to esp8266;
		Physical Connection: esp32 connects to the fan and the esp8266 (VIN: 3.3；GND: GND; esp8266 D2: GPIO 5; IN-A: GPIO 4;)	
	ble_tem_b_test_upload_reconnect_esp8266(esp8266)
		Function: Upload the data to MQTT;
		Physical Connection: esp32 connects to the CPE wirelessly;
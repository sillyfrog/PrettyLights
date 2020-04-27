# PrettyLights
Configure and control RGB and LED lights from an ESP8266

This is what I'm using to control the RGB (SK6812) LED's, and micro LED's (via a TLC5947 board).

It's configured for a device with 4MB or more of flash. I use the Wemos D1 Mini (not D1 mini lite).

Everything is built using PlatformIO in VScode. Firstly create a `src/wificredentials.h` file with your WiFi setup.

To install, do the initial build in VScode/PlatfromIO and upload via USB/Serial. Then upload the filesystem (PlatformIO: Upload filesystem image). Once this is done, you can browse to the IP address of the device. The first thing to configure is the number of RGB LED's and number of TLC5947 boards (PWM Boards).

As of writing, it supports up to 3 separate strips of RGB LED's, and 2x chained TLC5947 boards. On the D1 mini, the RGB strips are connected to D1, D2 and D8 respectively. For the TLC5947 boards, the first board is connected to D5/D6/D7 (Din/CLK/LAT). These boards can be chained, so that's the recommended way. However if you need a second chain, this is connected to D5/D6/D3 (Din/CLK/LAT) - Din and CLK are shared. If using a TLC5947 board, I recommend joining the `LAT` and `/OE` pins to minimize noticeable flicker.

Once the number of boards and strips is configured, reboot the ESP, and reload the browser.

Once configured and connected, each LED can be configured via the web interface. Click on "Edit Scheme" to edit what the LED will do. It'll try and update in realtime, but this is _not_ saved to the flash until you click "Save State".

To locate an LED, click on "Identify", this will flash the LED for 10 seconds, blanking everything else, so you know what it is. You can then name the LED by clicking on the text area (a grey line if there is nothing configured).

Bulk editing is also supported, click on the rows you want to update (on the number on the left is easiest), then click "Bulk Edit". When "Save Changes" is clicked, the selected LED's are also updated. Remember to "Save State" to store this to Flash and see what it looks like.

There are also various test modes at the end to check everything is working. I would suggest starting with "Test Mode 1" to check both RGB and TLC5947 (PWM) LED's.
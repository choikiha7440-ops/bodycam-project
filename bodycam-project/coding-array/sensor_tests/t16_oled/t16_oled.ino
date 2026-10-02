// [출력] OLED 화면 점검 - I2C (라이브러리: U8g2)
// 확장 쉴드 밑 "A <-> I2C" 스위치를 I2C 로
#include <U8g2lib.h>
#include <Wire.h>
U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

void setup() {
  Serial.begin(9600);
  oled.begin();
  Serial.println("OLED 점검: 화면에 글자와 숫자가 나오면 정상");
}

void loop() {
  oled.firstPage();
  do {
    oled.setFont(u8g2_font_ncenB10_tr);
    oled.drawStr(0, 14, "OLED TEST");
    oled.drawFrame(0, 20, 128, 20);
    oled.drawBox(2, 22, (millis() / 50) % 124, 16);
    oled.setFont(u8g2_font_6x10_tr);
    oled.setCursor(0, 60);
    oled.print("time: ");
    oled.print(millis() / 1000);
    oled.print(" s");
  } while (oled.nextPage());
  delay(100);
}

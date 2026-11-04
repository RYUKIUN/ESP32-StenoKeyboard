# Stenokeyboard (ESP32 + 2x MPR121 → Bluetooth keyboard)

steno ทั้งหมด (จับ chord, พจนานุกรม Plover, orthography, fingerspelling, ตัวเลข, undo/retro, คำสั่งลัด) ทำงานในตัว ESP32
ฝั่ง PC แค่ pair Bluetooth แล้วมองเห็นเป็นคีย์บอร์ดทั่วไป ไม่ต้องลงโปรแกรมใดๆ

> สำคัญ: เครื่องที่ผมใช้เขียนโค้ดไม่มี ESP-IDF จึง **ยังไม่ได้ build firmware ส่วน ESP32 จริง** ส่วน steno engine (ภาษา C ล้วน)
> ทดสอบเทียบกับ Plover ตัวจริงบนเครื่องแล้ว ~4,000 ลำดับสโตรกสุ่ม ผลตรงกัน ส่วน BLE/HID/MPR121 ต้องลองกับฮาร์ดแวร์จริง
> ถ้า build ไม่ผ่านหรือพฤติกรรมแปลก ส่ง log กลับมาได้เลย

## โครงสร้าง (แตก zip ลงโฟลเดอร์โปรเจกต์ ESP-IDF ได้เลย)

```
├── main/config.h        <- ตั้งค่าทั้งหมด (ขา I2C, ชื่อ Bluetooth, key map, ค่าจูนเซนเซอร์, ความเร็วพิมพ์)
├── main/main.c          <- ประกอบ component เข้าด้วยกัน
├── components/
│   ├── mpr121/ keypad/  <- ไดรเวอร์ MPR121 + อ่านปุ่ม (จาก phase 1)
│   ├── steno_core/      <- engine แบบพกพา (ไม่พึ่ง ESP-IDF): translator, formatter, orthography, dictionary reader
│   ├── steno_app/       <- chord detector + task ที่ป้อนสโตรกเข้า engine + mmap พจนานุกรมจาก flash
│   ├── hid_kbd/         <- แปลงข้อความ/คีย์ลัดเป็น HID report + คิวส่งแบบหน่วงเวลา
│   └── ble_hid/         <- BLE HID keyboard (NimBLE, bonding, Just Works)
├── dict/dict.bin        <- พจนานุกรมที่แปลงแล้ว (สร้างสำเร็จรูปมาให้)
├── tools/               <- convert_dict.py (สร้าง dict.bin), user.json (คำสั่งของคุณ), assets/ (main.json ของ Plover), hosttest/ (ชุดทดสอบเทียบ Plover)
├── partitions.csv  sdkconfig.defaults  CMakeLists.txt
```

## ก่อน flash ต้องเช็กอะไรบ้าง

1. **ขนาด flash = 4 MB** (ตั้งไว้ใน `sdkconfig.defaults` และ `partitions.csv`) ถ้าบอร์ดคุณใหญ่กว่า (8/16 MB) ใช้ได้เหมือนเดิม
   แต่ถ้าเล็กกว่า 4 MB ใช้ไม่ได้ ตรวจด้วย `esptool.py flash_id` (หรือ `idf.py flash` จะแจ้งตอนเริ่ม)
2. **ขา I2C และ address** ใน `main/config.h` หัวข้อ 2 (ค่าเริ่มต้นเหมือน phase 1):
   - โมดูลแรก = I2C0, SDA GPIO21, SCL GPIO22 = **มือขวา**
   - โมดูลสอง = I2C1, SDA GPIO18, SCL GPIO19 = **มือซ้าย**
3. **Key map** (หัวข้อ 5 ของ config.h) ตั้งตามที่คุณยืนยัน:

   | มือขวา (โมดูลแรก) | | | |
   |---|---|---|---|
   | e0 F | e1 P | e2 L | e3 T |
   | e4 R | e5 B | e6 G | e7 S |
   | e8 D | e9 Z | e10 E | e11 U |

   | มือซ้าย (โมดูลสอง, เลขเรียงกลับด้าน) | | | |
   |---|---|---|---|
   | e11 S | e10 T | e9 P | e8 H |
   | e7 S | e6 K | e5 W | e4 R |
   | e3 # | e2 * | e1 A | e0 O |

   ถ้า wiring จริงต่างไป แก้เฉพาะตาราง `CFG_KEYMAP`
4. **ค่าจูนเซนเซอร์** (touch/release threshold ฯลฯ) ใช้ค่าจาก phase 1 ที่ทดสอบแล้ว ถ้าปรับไว้ให้คัดลอกค่าเดิมมาใส่หัวข้อ 4
5. **ความเร็วพิมพ์**: `CFG_HID_REPORT_INTERVAL_MS` (ค่าเริ่มต้น 8 ms ≈ 60 ตัวอักษร/วินาที) ถ้า Windows ทำตัวอักษรหล่น/เพี้ยนให้เพิ่มค่า

## Build และ flash

```bash
. $IDF_PATH/export.sh
cd <โฟลเดอร์โปรเจกต์>
idf.py set-target esp32
idf.py build flash monitor
```

`idf.py flash` จะเขียน `dict/dict.bin` ลง partition `dict` ให้อัตโนมัติ (ผ่าน `main/CMakeLists.txt`)
ถ้า build แจ้งว่าแอปใหญ่เกิน partition `factory` (1.25 MB) ให้บอกผม จะย้ายขอบ partition ให้ ซึ่งแก้ได้ใน `partitions.csv` (dict ใช้ ~2.2 MB จาก 2.7 MB ที่จอง)

เขียนเฉพาะพจนานุกรมใหม่โดยไม่ build ใหม่ (หลังแก้ `tools/user.json`):

```bash
pip install lz4
python tools/convert_dict.py                    # สร้าง dict/dict.bin ใหม่ (user.json > commands.json > main.json)
python $IDF_PATH/components/partition_table/parttool.py --port COMx write_partition --partition-name dict --input dict/dict.bin
```

หรือ `idf.py flash` ก็เขียนใหม่ให้เหมือนกัน

## Pair กับ Windows

1. เปิดบอร์ดด้วย USB (log จะขึ้น `advertising as "Steno Keyboard"`)
2. Settings → Bluetooth & devices → Add device → Bluetooth → เลือก **Steno Keyboard** (Just Works ไม่ต้องใส่รหัส)
3. หลัง pair ครั้งแรก ต่อใหม่อัตโนมัติทุกครั้งที่เสียบไฟ (keys เก็บใน NVS)
4. ถ้าลบอุปกรณ์ใน Windows แล้ว pair ใหม่ ESP32 ยอมรับเองโดยไม่ต้องล้างอะไร ถ้ายังติดปัญหา ตั้ง `CFG_BLE_CLEAR_BONDS_ON_BOOT 1` flash ครั้งเดียว แล้วตั้งกลับเป็น 0

**ข้อควรรู้เรื่อง layout**: คีย์บอร์ดส่ง "ตำแหน่งปุ่ม" แบบ US ดังนั้นภาษาที่ใช้พิมพ์ใน Windows ต้องเป็น **English (US)** ตอนใช้ steno
ถ้าสลับเป็นภาษาไทยอยู่ ตัวอักษรจะออกเป็นอักษรไทยตามตำแหน่งปุ่ม

## ฟีเจอร์ steno

- พจนานุกรม Plover main.json (~147k รายการ) + commands.json + `tools/user.json`
- **Orthography** (กฎ -ing/-ed/-s/-er ฯลฯ และ suffix keys `-Z -D -S -G`) ตามกฎและ wordlist ของ Plover
- **Fingerspelling** (`A*`, `PW*`, ... และตัวพิมพ์ใหญ่ `A*P`...), **ตัวเลข** (`#` + คีย์, เลขต่อกัน)
- **Undo**: `*` ลบสโตรกล่าสุด (ย้อนได้ตาม `CFG_STENO_UNDO_LEVELS`)
- **Retro**: `R*FPB` สลับ `*` ของสโตรกก่อนหน้า, `TK*FS` ลบช่องว่างระหว่างสองคำก่อนหน้า, `TK*PLT` เติมช่องว่าง, `R*PT` ทำซ้ำสโตรกล่าสุด
- **เปิด/ปิด steno**: `PHRO*F` = suspend, `PHRO*PB` = resume, `PHROLG` = toggle (ตามพจนานุกรม Plover) ตอนปิดจะไม่พิมพ์อะไรเลย (ยกเว้นคำสั่งเปิดกลับ)
- **คำสั่งคีย์บอร์ด** (เพิ่มใน `tools/user.json`): copy `KO*P`, cut `K*UT`, paste `PA*PS`, select all `S*EL`, undo `*UPB`, redo `R*EUPB`, save `SA*EUFP`, find `TP*EUPBD` (แก้/เพิ่มได้ที่ `tools/user.json` แล้วรัน convert_dict.py) และคำสั่งใน commands.json ของ Plover เช่น ลูกศร, Home/End, Alt+Tab (`TA*BT`), Enter (`SKW-BGS`) ฯลฯ
- ตัวอักษรที่ไม่ใช่ ASCII (เช่น ø, ™, “ ”) ถูกแปลงเป็นตัวใกล้เคียง (`(TM)`, `"`) หรือตัดรายการทิ้ง (4 รายการ) เพราะคีย์บอร์ด US พิมพ์ได้เฉพาะ ASCII

### สิ่งที่ยังไม่รองรับ (ไม่ได้อยู่ในพจนานุกรม main ของ Plover)

prefix strokes, `{=conditional}`, `{MODE:...}`, `{:macro}` ที่ผู้ใช้เขียนเอง, ตัวอักษรนอก ASCII

หมายเหตุ: stroke จะถูกส่งเมื่อ **ปล่อยปุ่มทุกปุ่มแล้ว** (แบบ Plover ปกติ)

## รูปแบบไฟล์พจนานุกรม (dict.bin, เวอร์ชัน 1)

header 64 ไบต์ (`STNO`) → ไดเรกทอรี block (20 ไบต์/รายการ) → block ละ ~4 KB แบบ front-coded + LZ4 → wordlist filter (xor filter 16-bit)
ใช้สำหรับค้นหาด้วย binary search และเช็กคำในกฎ orthography ขนาดรวม ~2.2 MB

## ทดสอบ engine บนคอมพิวเตอร์ (ไม่ต้องใช้บอร์ด)

```bash
pip install plover lz4   # ต้องมี Plover ไว้เทียบ
gcc -std=c11 -O1 -fsanitize=address,undefined -Icomponents/steno_core/include -Icomponents/steno_core \
    components/steno_core/steno_*.c tools/hosttest/host_main.c -o hostt
cd tools && python hosttest/diff_test.py ../hostt 1 300
```

## ลิขสิทธิ์

`tools/assets/main.json`, `commands.json` และ `american_english_words.txt` มาจาก Plover (GPLv2+) กฎ orthography และ logic ของ engine พอร์ตมาจาก Plover
ดังนั้นโปรเจกต์นี้ควรเผยแพร่ภายใต้ GPLv2 หรือใหม่กว่าหากจะแจกจ่าย

# murmulator-os
Murmulator OS v.2.2.1<br/>

# Hardware needed
Raspberry Pi Pico 2 (RP2350)<br/>
Sources are "in-progress" state and testing now only on Murmulator devboard with VGA/HDMI output.<br/>
Simplest Murmulator schema is availabele there: https://github.com/AlexEkb4ever/MURMULATOR_classical_scheme<br/>
![Murmulator Schematics](https://github.com/javavi/pico-infonesPlus/blob/main/assets/Murmulator-1_BSchem.JPG)
![VGA OR hdmi](./assets/vga_hdmi.jpg)

# This Archive
Extract MOS2 folder to your SD-Card to /MOS2 folder.

# M-OS commands
cls - clear screen<br/>
dir / ls [dir] - show directory content. Use Ctrl+C to interrupt.<br/>
rm / del / era [file] - remove file (or empty directory)<br/>
cd [dir] - change current directory<br/>
cp [file1] [file2] - copy file1 as file2<br/>
mkdir [dir] - create directory<br/>
cat / type [file] - type file. Use Ctrl+C to interrupt.<br/>
rmdir [dir] - remove directory (recurive)<br/>
elfinfo [file] - provide .elf file info<br/>
psram - provide some psram info. Use Ctrl+C to interrupt.<br/>
swap - provide some swap info. Use Ctrl+C to interrupt.<br/>
sram - reference speed of swap base SRAM. Use Ctrl+C to interrupt.<br/>
cpu - show current CPU freq. and dividers, `cpu [NNN]` - change freq. to NNN MHz (it may hang on such action)<br/>
mem - show current memory state<br/>
set - show or set environment variables<br/>
mode [#] - set video-mode, for now it is supported:<br/>
<li>
 <ul>0 - 53x30, 1 - 80x30, 2 - 100x37, 3 - 128x48, 4 - 256x256x2-bit, 5 - 512x256x1-bit, 6 - 320x240x4-bit, 7 - 320x240x8-bit, 8 - 640x480x4-bit - for VGA</ul>
 <ul>0 - 53x30 and 1 - 80x30, 2 - 320x240x4-bit for HDMI</ul>
 <ul>0 - 53x30 for TV (RGB)</ul>
</li>
less - show not more than one page of other command in pipe, like `ls | less`. Use Ctrl+C for exit.<br/>
hex [file]/[@addr] - show file or RAM as hexidecimal dump. Use Ctrl+C to interrupt.<br/>
tail [-n #] [file] - show specified (or 10) last lines from the file. Use Ctrl+C to interrupt.<br/>
usb [on/off] - start a process to mount murmulator CD-card as USB-drive (NESPAD [B] button in mc)<br/>
mc - Murmulator Commander, use [CTRL]+[O] to show console, and [CTRL]+[Enter] for fast type current file path<br/>
mcview [file] - Murmulator Commander Viewer<br/>
mcedit [file] - Murmulator Commander Editor<br/>
mv [from_file_name] [to_file_name] - move/rename the file<br/>
gmode [#] - simple graphics mode test<br/>
font [width] [height] - show/set font size for graphics modes, like `font 6 8`<br/>
blimp [n1] [n2] - simple sound test. [n1] number of cycles, [n2] OS ticks between high and low levels (1/freq.)<br/>
wav [file] - simple .wav files player (tested on 8 kHz 1-channel 16-bit files). Use Ctrl+C to interrupt.<br/>
basic [file] - tiny basic interperator implementation (by Stefan Lenz, see https://github.com/slviajero/tinybasic for more info). Use Ctrl+C to interrupt.<br/>
ps - list of "processes" (FreeRTOS tasks).<br/>
kill [n] - send SIGKILL to a "process" (FreeRTOS task), [n] - task number returned by the `ps` utility.<br/>
dhrystone [n] [kHz] - small performance test (see: https://github.com/DnCraptor/arm_benchmarks)<br/>
whetstone [n] - MIPS (whetstone) double precision performance test (see: https://github.com/DnCraptor/arm_benchmarks)<br/>
linpack [n] - Kflops single precision performance test (see: https://github.com/DnCraptor/arm_benchmarks)<br/>
<br/>
[cmd] &gt; [file] - output redirection to file<br/>
[ENTER] - start command / flash and run .uf2 file in "demo" format (NESPAD [A] button in mc)<br/>
[TAB] - autocomplete<br/>
[BACKSPACE] - remove last character<br/>
[CTRL]+[SHIFT] - rus/lat<br/>
[CTRL]+[ALT]+[DEL] - reset (NESPAD [SELECT]+[B])<br/>
[CTRL]+[TAB]+[+] - increase CPU freq.</br>
[CTRL]+[TAB]+[-] - decrease CPU freq.</br>
[ALT]+[0-9]+[0-9]+[0-9] - manual enter some character by its decimal code (CP-866 codepage)<br/>
[ALT]+[Enter] - try to use flasg instead of RAM to launch application (from `mc`)<br/>
<br/>
# M-OS system variables
BASE - base directory with commands implementations<br/>
SWAP - swap settings<br/>
COMSPEC - a path to command interpretator<br/>
PATH - list of directories to lookup for applications<br/>
GMODE - set initial graphics mode<br/>
TEMP - specify a folder with temporary files<br/>
CD - defualt path for cd command<br/>

# Boot-loader mode
If uf2 application was started from M-OS, and such application is not designed for M-OS, it is possible to return to M-OS only via reboot:<br/>
Press [F10] (DPAD [START]+[SELECT]) and hold on the Murmulator reset or power-on, in this case uf2 application will be repaired you will return to last saved on Flash.<br/>
Press [F11] or [SPACE] (DPAD [SELECT]) and hold on the Murmulator reset or power-on, in this case uf2 application startup will be skip and you will return to the M-OS. And in case DPAD [B] button is also hold, it automounts SD-Cart as USB-drive.<br/>
Press [F12] or [ENTER] (DPAD [START]) and hold on the Murmulator reset or power-on, in case you want to start USB-drive mode prior starting M-OS.<br/>
Press [TAB] (DPAD [A]) and hold on the Murmulator reset or power-on, in case you want to replace default output by seconday driver.<br/>
Press DPAD [B] and hold on the Murmulator reset or power-on, in case you want to switch default output to third output.<br/>
Press [HOME] and hold on the Murmulator reset or power-on, in case you want  start USB-drive mode prior starting M-OS.<br/>
Press [ESC] and hold on the Murmulator reset or power-on, to ignore .firmware and force boot to MOS.<br/>
Press and hold any other key on the Murmulator reset or power-on, to boot (skip waiting for other key-cases).<br/>

# config.sys
Use it to override for "M-OS system variables" or:<br/>
CPU - to overclock or downclock the system on start. It may be useful to set some less freq. in case your board unable to support default<br/>
Example:<br/>
CPU=352<br/>
It forces to change CPU frequency to 352 MHz to avoid overload<br/>
N.B. Default volage for RP2350 in MOS is 1.6V (not yet to be reconfigured)<br/>

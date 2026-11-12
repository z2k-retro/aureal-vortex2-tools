# Using the Vortex 2 emulation in 86Box

The [`vortex2` branch of z2k-retro/86Box](https://github.com/z2k-retro/86Box/tree/vortex2) adds an emulated **Aureal Vortex 2 (SQ2500)** PCI sound card with hardware A3D 2.0. It runs Aureal's own Windows 98 drivers, so games get the same positional audio, reflections and speaker processing as on the real card.

## Download

Windows builds are on the [releases page](https://github.com/z2k-retro/86Box/releases) under the `vortex-` tags. The zips are named `86Box-PowerVR…` because the branch also carries the PowerVR PCX2 emulation; both devices are in every build. To build it yourself, check out the `vortex2` branch and follow 86Box's usual build instructions.

## Setting up a machine

1. In the machine's settings, choose **Aureal Vortex 2 (SQ2500)** as the sound card. Any machine with PCI slots works; a Pentium II or faster suits the games that used A3D.
2. In Windows 98, install Aureal's driver package for the SQ2500. The emulation was built and tested against driver **4.06.2048**; [Phil's Computer Lab](https://www.philscomputerlab.com/aureal-vortex-2.html) has it.
3. In Aureal's control panel, set the speaker type to match how you listen: **Headphones**, **Satellite speakers** (the default) or **Monitors**. With speakers, the card's crosstalk canceller adds cues meant for two speakers in front of you; on headphones it sounds odd, so pick Headphones there. Four-speaker output is supported too (see the settings below).
4. Games that support A3D usually have an A3D option in their sound settings. Aureal's demos in the driver package are a good first test.

## Device settings

| Setting | Default | What it does |
| --- | --- | --- |
| Mix rear channels into the stereo output | Off | The SQ2500 has a second (rear) output. 86Box plays stereo, so in quad mode the rear channels are lost unless this is on. Turn it on if you set the control panel to four speakers but listen in stereo |
| CD audio input level | −12 dB | How loud the CD drive's analog audio reaches the card, against the codec's full scale. Real drives and cards varied; the default matches the music-to-game balance of Half-Life's current release. Move a step if CD music sounds too loud or too quiet against game sound |
| Write diagnostic log | Off | Writes `vortex2.log` in the machine's folder: a status line every two seconds plus stream, routing and 3D-source events. Use it when reporting a problem |
| Write register trace | Off | Writes `vortex2-trace.txt` (every register access, in the format of the real-card sessions) and `vortex2-dma.bin` (every block of sample data the chip fetched) in the machine's folder. `bin/v2replay -d` replays them offline into the model, reproducing the audio exactly, with any one source soloed. The files grow by tens of megabytes a minute, so turn it on only for a short run |

## What is emulated

- DirectSound playback, including hardware buffers with volume, pan and frequency control, through the card's sample-rate converters and mixer. The converters filter like the real card's, so low-rate game sounds keep the card's clean character.
- A3D 2.0 in hardware: HRTF filters, interaural delay, distance and air-absorption filtering, occlusion, and the glides between positions, all matched to measurements of a real SQ2500. Reflections and the geometry engine run in Aureal's driver, as on the real card.
- The speaker crosstalk canceller for headphones, satellites and monitors, and the hardware EQ.
- The wavetable voices the driver uses for reflections and extra sounds.
- MIDI through the "Vortex AU8830 Wavetable" device. This is Aureal's software synthesiser running in the driver, so it sounds exactly as on the real card.
- CD audio. On the SQ2500 the drive's analog cable feeds the codec, and the driver records it and mixes it digitally, so game soundtracks on audio CD tracks play, and the Windows CD volume control works as on a real card. In 86Box, put the game CD (a BIN/CUE with its audio tracks) in the emulated CD-ROM drive.
- Sound Blaster Pro emulation in Windows DOS boxes, which Aureal's driver also does in software.

## Known limitations

- **Sound Blaster emulation can freeze the machine at boot.** It reserves IRQ 5, DMA 1 and port 220h, and Windows will not let you change them. If Windows has put a PCI device on IRQ 5 (on the ASUS P2B-LS it was the USB controller), the machine can hang right after the startup sound. Disable "Vortex AU8830 Sound Blaster Pro Emulation" in Device Manager, or move the other device off IRQ 5. A real SQ2500 behaves the same way. For DOS games an emulated Sound Blaster 16 is the better choice anyway: the Vortex's Sound Blaster support only reaches SB Pro and only works inside Windows.
- **Aureal's A3D splash screen** can crash PowerVR games when they start (Unreal on a Matrox m3D, for example). It is an interaction with PowerVR's start-up, not the sound card; turning the splash screen off (the `SplashScreen` value under `Software\Aureal\A3D` in the registry) fixes it.
- Not emulated yet: recording from the microphone or line in (only the CD input reaches the codec), the game port and MPU-401 port, real-mode DOS outside Windows, and S/PDIF.
- Only the Windows 98 driver has been tested. Other driver versions and the Windows 2000/XP drivers may work but are untested.

## Reporting a problem

Open an issue on [z2k-retro/86Box](https://github.com/z2k-retro/86Box/issues) with:

- the game or program, what you did, and what sounded wrong;
- the machine configuration (machine type, CPU, video card) and the driver version;
- `vortex2.log` with **Write diagnostic log** turned on;
- if you can, a short recording of the problem (86Box's own audio, captured with any recording tool).

The log shows whether the driver set up streams and 3D sources, whether interrupts were being serviced, and the left/right levels at each stage, which is usually enough to tell a sound-card problem from something else.

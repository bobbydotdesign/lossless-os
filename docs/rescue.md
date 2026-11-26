# Rescue: a player that won't start

For an H2 that shows nothing at power-on, or only the blue light, after
installing, updating or uninstalling. Your music on the card is safe:
nothing here touches it.

The player's chip has a USB boot mode built into it. Nothing can erase it,
so this always works. From a computer, the rescue kit starts a small
program in the player's memory, which writes a bootloader back into the
player's bootloader partition and nowhere else, then checks it.

You need:

- A Mac or a Linux computer, and the USB cable.
- `lossless-os-<version>-h2-rescue.zip` from the
  [releases](https://github.com/bobbydotdesign/lossless-os/releases).
- The bootloader to write. To go back to the original firmware, that's
  HiFi Walker's firmware update for your player's version, from
  [their download page](https://hifiwalker.com/pages/download) (the `.upt`
  file; [install.md](install.md) lists the V2.3 one's SHA-256). To put
  Lossless OS's bootloader back instead, the release's `update.upt`.

## Steps

1. Unzip the rescue kit.
2. Install what it builds with:
   - **Mac:** Apple's command line tools and libusb. In Terminal:
     `xcode-select --install`, then with [Homebrew](https://brew.sh):
     `brew install libusb pkg-config`.
   - **Linux:** a C compiler, make and libusb (Debian and Ubuntu:
     `sudo apt install build-essential libusb-1.0-0-dev pkg-config`). Run
     the next step with `sudo`, or allow USB device `a108:1000`.
3. In Terminal, in the kit's folder:

   ```
   ./h2-rescue.sh ~/Downloads/H2-Firmware-V2.3.upt
   ```

   It checks the file first and says which bootloader it found.
4. When it says so: switch the player off, hold **Menu** and plug it into
   the computer. Keep holding Menu for about five seconds. The blue light
   comes on and the screen stays dark: that's the USB boot mode.
5. The script sets up the player's memory, tests it, and starts the
   writer. In under a minute the player's screen says "Bootloader written
   and checked": the writer read the whole bootloader back and it
   matched. If it says "Not written" instead, unplug it and run the
   script again: the player can always be reached this way.
6. Hold **Power** until the screen goes off, unplug it, and switch it on.
   With the original bootloader back, it starts the original firmware as
   it came.

If the script says the player's memory didn't start, or there's no
answer: unplug, wait for the light to go out, and run it again. Nothing is
written until the memory has been set up and tested.

## Windows

The kit doesn't run on Windows yet. Ask in an issue: a friend with a Mac
or a Linux computer (or a Linux live USB) can do it in a few minutes.

## Still stuck?

Open an [issue](https://github.com/bobbydotdesign/lossless-os/issues)
with your player's original firmware version, what the screen shows, and
what the script printed.

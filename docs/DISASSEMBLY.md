# Camera dissasembly

In order to run custom firmware on the Silvergear camera, dissasembly is required, so this guide covers how to open the device up (for both units: Vision and Silvergear)

Before doing any disassembly, remove the microSD card from your device and disconnect USB power.

## Required tools

Both will share the same required tools to open the device up.

- Philips PH00 screwdriver (with a long outer end and ideally small to fit in tight corners)
- Twizzers
- Thin metal spudger
- Plastic prying tool

You can get these tools usually in phone repair kits.

## Steps (Silvergear)

### Step 1

Open the paper drawer (remove any thermal paper if present) and find 2 Philips screws in each corner as shown here:

<img src="../images/disassembly/printer-cover-screws.jpg" width="480" alt="Printer paper cover showing 2 Philips screws on Silvergear">

Remove them using a small Philips screwdriver (margins are thin that a normal Philips screwdriver won't fit them)

### Step 2

Carefully pry the top plastic cover using a thin metal spudger starting at the corner where the Philips screws are present and going in the corners one by one, ensuring you don't damage the outer plastic shell as shown here.

> [!WARNING]
> Ensure that you do this process slowly as to not damage the thin flex ribbon connecting to the back camera!

<img src="../images/disassembly/camera-shell-prying.jpg" width="480" alt="Camera back cover getting pryed using a thin metal spudger, starting at the bottom corner">

<img src="../images/disassembly/camera-shell-prying-stage-2.jpg" width="480" alt="Camera back cover getting pryed using a thin metal spudger, prying now above the bottom corner">

<img src="../images/disassembly/camera-shell-prying-stage-3.jpg" width="480" alt="Camera back cover getting pryed using a thin metal spudger, prying the other side of the cover">

<img src="../images/disassembly/camera-shell-prying-stage-4.jpg" width="480" alt="Camera back cover getting pryed using a thin metal spudger, prying now above the bottom corner at the other side">

<img src="../images/disassembly/camera-shell-prying-stage-5.jpg" width="480" alt="Camera back cover getting pryed using a thin metal spudger, prying now near the capture and power button">

Once all corners are pryed open, carefully lift the cover ensuring the rear camera ribbon is undamaged as seen here:

<img src="../images/disassembly/camera-shell-opened.jpg" width="480" alt="Camera back cover opened up, revealing the printer assembly, battery and the thin camera flex ribbon">

### Step 3

Disconnect the rear camera ribbon using twezzers, prying at the black connector tab against you, as shown here:

<img src="../images/disassembly/camera-rear-camera-connector-closed.jpg" width="480" alt="Camera rear camera connector (connector closed)">

<img src="../images/disassembly/camera-rear-camera-connector-open.jpg" width="480" alt="Camera back cover opened up, revealing the printer assembly, battery and the thin camera flex ribbon open">

Then you can safely remove the rear camera flex ribbon, stepping the rear cover aside.

<img src="../images/disassembly/camera-rear-camera-connector-open.jpg" width="480" alt="Camera back cover opened up, revealing the printer assembly, battery and the thin camera flex ribbon open">

### Step 4

Remove the battery by carefully prying it using a plastic spudger (it's attacted using double sided tape)

> [!WARNING]
> Ensure that you do this process slowly as to not damage the thin flex ribbon connecting to the printer!

<img src="../images/disassembly/camera-rear-prying-battery.jpg" width="480" alt="Camera back cover opened up, pyring against the battery using a plastic spudging tool">

Once the battery is pryed from the shell, carefully remove the stuck flex ribbon attacted to the battery and disconnect it:

<img src="../images/disassembly/camera-rear-battery-out-from-shell.jpg" width="480" alt="Camera back cover opened up, battery is now outside the shell">

### Step 5

After the battery is removed, remove the printer assembly (unscrew 2 Philips screws) and carefully step it aside.

<img src="../images/disassembly/printer-assembly-screws.jpg" width="480" alt="Printer assembly being helt by 2 screws">

<img src="../images/disassembly/printer-assembly-unscrewed.jpg" width="480" alt="Printer assembly unscrewed, being loose">

Then remove the flex ribbon (same prying of the black tab) connecting to the camera PCB as shown here:

<img src="../images/disassembly/printer-assembly-flex-connected.jpg" width="480" alt="Printer assembly being helt by 2 screws">

<img src="../images/disassembly/printer-assembly-flex-disconnected.jpg" width="480" alt="Printer assembly unscrewed, being loose">

### Step 6

Remove the 3 screws holding the assembly where the battery and printer reside as shown here using a thin Philips screwdriver.

<img src="../images/disassembly/camera-printer-bracket-screws-right-side.jpg" width="480" alt="Bracket where printer assembly and battery are mounted being helt by 3 screws (this side shows 2 screws)">

<img src="../images/disassembly/camera-printer-bracket-screws-left-side.jpg" width="480" alt="Bracket where printer assembly and battery are mounted being helt by 3 screws (this side shows 1 screw)">

Then remove the bracket by lifting it up:

<img src="../images/disassembly/camera-printer-bracket-lifting.jpg" width="480" alt="Bracket where printer assembly and battery are mounted being lifted up">

> [!NOTE]
> Removing this bracket will also make the printer cover fall out as shown here, so during reassembly put this cover back in to ensure you can load paper properly:
> 
> <img src="../images/disassembly/camera-printer-cover-fallen-off.jpg" width="480" alt="Printer paper cover falled off during removal of the bracket where printer assembly and battery are mounted">
>
> Also, the speaker will be mounted with double sided tape, so remove the speaker by carefully prying it before removing the bracket and disconnect it from the PCB if needed:
> 
> <img src="../images/disassembly/camera-speaker-prying.jpg" width="480" alt="Tiny speaker where printer assembly and battery are mounted stuck using double sided tape being pryed using a screwdriver">


### Step 7.

At this point, you are close to accessing the SPI flash and the SoC, however you will still need to remove 2 screws holding the main PCB to the front camera shell and one screw holding the front camera.

First lift the black tab on the connector holding the screen ribbon, then disconnect the flex ribbon and step it aside as shown here:

<img src="../images/disassembly/camera-screen-connector-open.jpg" width="480" alt="Camera screen flex connector tab being open to disconnect the screen">

Then remove the Philips screw holding the front camera bracket to the front side of the camera shell.

After this, remove the 2 Philips screws holding the main PCB to the front side of the camera shell and carefully lift the PCB from the right side as shown here:

<img src="../images/disassembly/camera-pcb-being-lifted.jpg" width="480" alt="PCB previously being attached to the front camera shell being lifted">

After it's lifted, you should finally be able to access the SPI flash (flip PCB to see the SPI flash and main SoC) to be able to flash DOOM.

<img src="../images/disassembly/camera-pcb-removed.jpg" width="480" alt="PCB removed from front camera shell">

## Steps (Vision)

### Step 1. 

Remove the 4 front Philips screws as shown here using a Philips screwdriver:

<img src="../images/disassembly/vision-camera-front-screws.jpg" width="480" alt="Vision camera showing 4 visible Philips screws to unscrew from">

### Step 2. 

Then, remove the sticker holding the product information (like shown below) in order to remove the back shell:

<img src="../images/disassembly/vision-camera-product-sticker.jpg" width="480" alt="Vision camera showing the product sticker, which needs to be removed for disassembly">

Once done, carefully open the rear cover like a book (front shell is opened to the left side), ensuring you don't damage the rear camera flex ribbon or the speaker wires.

<img src="../images/disassembly/vision-camera-opening-back-shell.jpg" width="480" alt="Vision camera being opened like a book from the left side">

After opening, it should look like this:

<img src="../images/disassembly/vision-camera-opened.jpg" width="480" alt="Vision camera opened like a book">

### Step 3. 

Open the black tab holding the rear camera flex ribbon and remove it as shown here:

<img src="../images/disassembly/vision-camera-camera-connector-flex-open.jpg" width="480" alt="Vision camera opened like a book, with the thin camera flex ribbon connector open">

Then, carefully pry the rear mounted speaker using tweezers (speaker is mounted via double sided tape) and remove it:

<img src="../images/disassembly/vision-camera-speaker-getting-pryed.jpg" width="480" alt="Vision camera opened like a bookremoved, with tweezers prying the rear mounted speaker">

After this, you can put the back cover aside.

### Step 4. 

Open the black tab holding the screen flex ribbon and remove it as shown here:

<img src="../images/disassembly/vision-camera-screen-connector-flex-open.jpg" width="480" alt="Vision camera PCB with the screen flex ribbon connector tab open">

Then, remove the battery (held down with double sided tape) and remove 2 Philips screws near the micro SD card slot and USB-C port using a Philips screwdriver:

<img src="../images/disassembly/vision-camera-battery-getting-pryed.jpg" width="480" alt="Vision camera PCB with the battery getting pryed using a plastic spudger">

Finally, carefully lift the PCB as shown here:

<img src="../images/disassembly/vision-camera-pcb-getting-lifted.jpg" width="480" alt="PCB previously being attached to the front camera shell being lifted">

After it's lifted, you should finally be able to access the SPI flash (flip PCB to see the SPI flash and main SoC) to be able to recover the device if a bad flash were to happen.

<img src="../images/disassembly/vision-camera-pcb-removed.jpg" width="480" alt="PCB removed from front camera shell">

## Reassembly

The process can be also followed backwards for both cameras (carefully liting back the PCB into the desired flace, plugging flex ribbons back in, screwing everything back together), so follow the steps in reverse (replacing steps to for example plug things like flex connectors back in) to assemble it back together.


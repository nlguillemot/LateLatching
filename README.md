# Late Latching

Comparing passing mouse coords to shader by late latching vs. by glUniform.

The goal of this experiment is to try to have less latency in a rendered mouse cursor by reading the mouse coordinate at the last second possible.

This is done by writing the mouse coordinate to a persistent GPU buffer as soon as the OS event comes in, rather than uploading it once per frame.

Every time a new input comes in, it gets pushed to a ring buffer. When the vertex shader executes to display the cursor, it reads from the most recent entry of that ring buffer.

All the relevant source code is inside [main.cpp](main.cpp). The shaders are string literals.

You can test the difference by taking a screenshot while dragging your mouse quickly. The default Windows screenshot feature doesn't capture the OS cursor, so you need to use some special screenshotting software if you want the OS cursor to show up in the image comparison.

Thanks to Timothy Lottes for the initial inspiration.

Thanks to Per Vognsen for suggesting to add a mode that compares all techniques, and a tweakable delay before the start of each frame.

Thanks to Ian Ashmore for the screenshot below taken on NVIDIA GTX 1060.

Thanks to Per and Michael Cameron for the discussion that inspired adding a fullscreen toggle.

## Sample Image

<img src="http://i.imgur.com/otnYpmL.png" width="600">

<img src="http://i.imgur.com/4IiKDB0.jpg" width="600">

# Late Latching

Comparing passing mouse coords to shader by late latching vs. by glUniform.

The goal of this experiment is to try to have less latency in a rendered mouse cursor by reading the mouse coordinate at the last second possible.

This is done by writing the mouse coordinate to a persistent GPU buffer as soon as the OS event comes in, rather than uploading it once per frame.

Every time a new input comes in, it gets pushed to a ring buffer. When the vertex shader executes to display the cursor, it reads from the most recent entry of that ring buffer.

All the relevant source code is inside [main.cpp](main.cpp). The shaders are string literals.

## Sample Image

<img src="http://i.imgur.com/IsEr2SG.png" width="600">

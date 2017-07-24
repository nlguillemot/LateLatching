# Late Latching

Comparing passing mouse coords to shader by late latching vs. by glUniform.

The goal of this experiment is to try to have less latency in a rendered mouse cursor by reading the mouse coordinate at the last second possible.

This is done by writing the mouse coordinate to a persistent GPU buffer as soon as the OS event comes in, rather than uploading it once per frame.

## Sample Image

<img src="http://i.imgur.com/IsEr2SG.png" width="600">

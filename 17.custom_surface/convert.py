#!/usr/bin/env python

from PIL import Image
import shutil
import struct

file_src = "3.rgb"
file_in = "test.png"

#shutil.copyfile(file_src, file_in)


img = Image.open(file_in)
raw_img = "test.txt"
raw_img_fd = open(raw_img, "wb")

raw_img_pixel = img.convert('RGB');

for i in range(827):
    for j in range(646):
        r, g, b = raw_img_pixel.getpixel((i, j))
        print(r, g, b)
        R=struct.pack('B', r)
        G=struct.pack('B', g)
        B=struct.pack('B', b)
        A=struct.pack('B', 0)
        raw_img_fd.write(B)
        raw_img_fd.write(G)
        raw_img_fd.write(R)
        raw_img_fd.write(A)

raw_img_fd.close()

file_out = "test1.bmp"


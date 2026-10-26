#!/usr/bin/env python3
"""Usage: sheet.py <level> <every_n_frames> <first_frame> <count> -> shots/sheet.png"""
import sys, glob, subprocess, os
from PIL import Image
lv, every, first, count = map(int, sys.argv[1:5])
for f in glob.glob('shots/l*.ppm'): os.remove(f)
subprocess.run(['./build/host', 'bot', str(lv), str(lv), str(every)], stdout=subprocess.DEVNULL)
fs = sorted(glob.glob(f'shots/l{lv}_*.ppm'))
fs = [f for f in fs if int(f[-8:-4]) >= first][:count]
W, H = 320, 240
cols = 4
sheet = Image.new('RGB', (W * cols // 2, H * ((len(fs) + cols - 1) // cols) // 2))
for i, f in enumerate(fs):
    sheet.paste(Image.open(f).resize((W // 2, H // 2)), ((i % cols) * W // 2, (i // cols) * H // 2))
sheet.save('shots/sheet.png')
print(len(fs), 'frames')

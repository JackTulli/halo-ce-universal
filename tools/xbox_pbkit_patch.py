"""Applies the Halo port's pbkit changes to nxdk/lib/pbkit (pbkit.c, pbkit.h):
the frame buffer count (2 for double buffering) and the depth buffer
(skipped: the application has its own) chosen before pb_init, and a query
for a flip still waiting for the vertical blank."""

import sys
from pathlib import Path

nxdk = Path(sys.argv[1])
source = nxdk / "lib/pbkit/pbkit.c"
header = nxdk / "lib/pbkit/pbkit.h"

text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"


def replace(old, new, count=1):
    global text
    old = old.replace("\n", nl)
    new = new.replace("\n", nl)
    found = text.count(old)
    if found != count:
        sys.exit(f"pbkit.c: expected {count} of {old!r}, found {found}")
    text = text.replace(old, new)


if "pb_BackBufferCountWanted" in text:
    print("pbkit.c: already patched")
else:
    replace("static  int         pb_BackBufferbReady[3]={0,0,0};\n",
            "static  int         pb_BackBufferbReady[3]={0,0,0};\n"
            "/* (halo-ce-universal) chosen before pb_init: 2 back buffers (triple\n"
            "buffering) or 1 (double), and whether pbkit allocates a depth buffer */\n"
            "static  int         pb_BackBufferCountWanted=2;\n"
            "static  int         pb_DepthBufferWanted=1;\n")
    replace("        next=(next+1)%3;\n        pb_BackBufferNxtVBL=next;\n",
            "        next=(next+1)%pb_FrameBuffersCount;\n        pb_BackBufferNxtVBL=next;\n")
    replace("            next=(next+1)%3;\n            pb_BackBufferNxt=next;\n",
            "            next=(next+1)%pb_FrameBuffersCount;\n            pb_BackBufferNxt=next;\n")
    replace("    pb_back_index=(pb_back_index+1)%3;\n",
            "    pb_back_index=(pb_back_index+1)%pb_FrameBuffersCount;\n")
    replace("    BackBufferCount=2;          //triple buffering technic!\n",
            "    BackBufferCount=pb_BackBufferCountWanted; //2: triple buffering technic! 1: double\n")
    replace("    DSAddr = (DWORD)MmAllocateContiguousMemoryEx(DSSize, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);\n"
            "\n"
            "    pb_DepthStencilAddr=DSAddr;\n"
            "    if (!DSAddr)\n"
            "    {\n"
            "        pb_kill();\n"
            "        return -11;\n"
            "    }\n"
            "\n"
            "    pb_DSAddr=DSAddr;\n"
            "\n"
            "    pb_assign_tile( 1,",
            "    if (!pb_DepthBufferWanted)\n"
            "    {\n"
            "        //the application has its own depth buffer: pbkit's depth window\n"
            "        //spans the frame buffers instead, through which nothing is written\n"
            "        pb_DepthStencilAddr=0;\n"
            "        pb_DSAddr=pb_FrameBuffersAddr;\n"
            "        pb_DSSize=pb_FBSize;\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "    DSAddr = (DWORD)MmAllocateContiguousMemoryEx(DSSize, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);\n"
            "\n"
            "    pb_DepthStencilAddr=DSAddr;\n"
            "    if (!DSAddr)\n"
            "    {\n"
            "        pb_kill();\n"
            "        return -11;\n"
            "    }\n"
            "\n"
            "    pb_DSAddr=DSAddr;\n"
            "\n"
            "    pb_assign_tile( 1,")
    replace("            0x84000001          //DWORD tile_flags (0x04000000 for 32 bits)\n"
            "            );\n"
            "\n"
            "\n"
            "    if (pb_ExtraBuffersCount)\n",
            "            0x84000001          //DWORD tile_flags (0x04000000 for 32 bits)\n"
            "            );\n"
            "    }\n"
            "\n"
            "\n"
            "    if (pb_ExtraBuffersCount)\n")
    replace("    memset((DWORD *)pb_DSAddr,0,pb_DSSize);\n",
            "    if (pb_DepthStencilAddr) memset((DWORD *)pb_DSAddr,0,pb_DSSize);\n")
    replace("//returns 1 if we have to retry later (means no free buffer, draw more details next time)\nint pb_finished(void)\n",
            "/* (halo-ce-universal) before pb_init: the number of back buffers (2, or 1\n"
            "for double buffering), and whether pbkit allocates its depth buffer */\n"
            "void pb_set_back_buffer_count(int count)\n"
            "{\n"
            "    if (count>=1 && count<=2) pb_BackBufferCountWanted=count;\n"
            "}\n"
            "\n"
            "void pb_set_depth_buffer(int wanted)\n"
            "{\n"
            "    pb_DepthBufferWanted=wanted;\n"
            "}\n"
            "\n"
            "/* (halo-ce-universal) whether a finished frame still waits for the\n"
            "vertical blank to be shown */\n"
            "int pb_flip_pending(void)\n"
            "{\n"
            "    return pb_BackBufferbReady[0] || pb_BackBufferbReady[1] || pb_BackBufferbReady[2];\n"
            "}\n"
            "\n"
            "//returns 1 if we have to retry later (means no free buffer, draw more details next time)\nint pb_finished(void)\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: patched")

# version 2: a frame counts as pending from pb_finished on, not only once the
# GPU reaches its end (the ready flag is set then, by the DPC): right after
# pb_finished nothing was flagged yet, and a double-buffered caller drew into
# the buffer on the screen (tearing at the bottom at 720p)
text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"
if "pb_FramesFinished" in text:
    print("pbkit.c: version 2 already")
else:
    replace("static  int         pb_DepthBufferWanted=1;\n",
            "static  int         pb_DepthBufferWanted=1;\n"
            "/* (halo-ce-universal) frames handed to pb_finished, and flips done */\n"
            "static  volatile DWORD pb_FramesFinished=0;\n"
            "static  volatile DWORD pb_FlipsDone=0;\n")
    replace("        next=(next+1)%pb_FrameBuffersCount;\n        pb_BackBufferNxtVBL=next;\n",
            "        next=(next+1)%pb_FrameBuffersCount;\n        pb_BackBufferNxtVBL=next;\n        pb_FlipsDone++;\n")
    replace("    return pb_BackBufferbReady[0] || pb_BackBufferbReady[1] || pb_BackBufferbReady[2];\n",
            "    return pb_FlipsDone!=pb_FramesFinished;\n")
    replace("    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_FINISHED); //subprogID PB_FINISHED: gets frame ready to show up soon\n",
            "    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_FINISHED); //subprogID PB_FINISHED: gets frame ready to show up soon\n"
            "    pb_FramesFinished++;\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: version 2 patched")

# version 3: a vertical blank the DPC handles itself (it arrived while the ISR
# had the interrupt masked, during the DPC's loop) rotated a ready buffer and
# counted the flip without re-pointing the display, which only the ISR did: a
# double-buffered caller then drew into the buffer on the screen. The handler
# now flips as well (the same write the ISR makes, harmless when it already
# did)
text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"
if "(halo-ce-universal) a blank the ISR missed" in text:
    print("pbkit.c: version 3 already")
else:
    replace("        //screen swapping has been done already, theoretically, in ISR\n"
            "        pb_BackBufferbReady[next]=0;\n",
            "        //screen swapping has been done already, theoretically, in ISR\n"
            "        //(halo-ce-universal) a blank the ISR missed (the interrupt was\n"
            "        //masked while the DPC ran) is flipped here\n"
            "        if (pb_debug_screen_active==0) VIDEOREG(PCRTC_START)=pb_FBAddr[pb_BackBufferIndex[next]]&0x03FFFFFF;\n"
            "        pb_BackBufferbReady[next]=0;\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: version 3 patched")

htext = header.read_text(encoding="latin-1")
hnl = "\r\n" if "\r\n" in htext else "\n"
if "pb_set_back_buffer_count" in htext:
    print("pbkit.h: already patched")
else:
    anchor = "DWORD   pb_wait_for_vbl(void);"
    if htext.count(anchor) != 1:
        sys.exit("pbkit.h: anchor not found")
    line_end = htext.index(hnl, htext.index(anchor)) + len(hnl)
    addition = ("void    pb_set_back_buffer_count(int count); //before pb_init: 2 (default) or 1" + hnl +
                "void    pb_set_depth_buffer(int wanted); //before pb_init: 0 skips pbkit's depth buffer" + hnl +
                "int     pb_flip_pending(void); //a finished frame still waits for the vertical blank" + hnl)
    htext = htext[:line_end] + addition + htext[line_end:]
    header.write_text(htext, encoding="latin-1", newline="")
    print("pbkit.h: patched")

# version 4: how many finished frames have not reached the screen yet. The
# next frame is drawn into the buffer of the frame (buffers - 1) before it,
# which must have left the screen; pbkit's own check (pb_finished's ready
# table) counts only frames the GPU has finished, so a caller that runs
# ahead of the display (the frame rate unlocked) drew into the frame on the
# screen or the one waiting for it
text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"
if "int pb_frames_pending(void)" in text:
    print("pbkit.c: version 4 already")
else:
    replace("    return pb_FlipsDone!=pb_FramesFinished;\n}\n",
            "    return pb_FlipsDone!=pb_FramesFinished;\n}\n"
            "\n"
            "/* (halo-ce-universal) finished frames not on the screen yet: handed to\n"
            "pb_finished and not yet flipped to */\n"
            "int pb_frames_pending(void)\n"
            "{\n"
            "    return (int)(pb_FramesFinished-pb_FlipsDone);\n"
            "}\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: version 4 patched")

htext = header.read_text(encoding="latin-1")
hnl = "\r\n" if "\r\n" in htext else "\n"
if "pb_frames_pending" in htext:
    print("pbkit.h: version 4 already")
else:
    anchor = "int     pb_flip_pending(void);"
    if htext.count(anchor) != 1:
        sys.exit("pbkit.h: version 4 anchor not found")
    line_end = htext.index(hnl, htext.index(anchor)) + len(hnl)
    htext = (htext[:line_end] + "int     pb_frames_pending(void); //finished frames not on the screen yet" + hnl +
             htext[line_end:])
    header.write_text(htext, encoding="latin-1", newline="")
    print("pbkit.h: version 4 patched")

# version 5: a push buffer the pusher found invalid data in stops the
# console, as a GPU error does: before, the report went up and the pusher
# skipped a dword and went on, and at 720p in the low 64 MB (the debug
# screen is the device's depth buffer too) the next frame's depth clear
# erased the report the user photographs
text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"
if "(halo-ce-universal) the pusher's report stays" in text:
    print("pbkit.c: version 5 already")
else:
    replace("        debugPrint(\"Dma push buffer engine encountered invalid data at these addresses.\\n\");\n",
            "        debugPrint(\"Dma push buffer engine encountered invalid data at these addresses.\\n\");\n"
            "        //(halo-ce-universal) the pusher's report stays: halted as for a GPU error\n"
            "        debugPrint(\"System halted\\n\");\n"
            "        while(1) {\n"
            "          Sleep(2000);\n"
            "        };\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: version 5 patched")

# version 6: pb_finished may leave pbkit's own colour and blit DMA objects
# alone. Re-targeting them for the next buffer (set_draw_buffer) takes eight
# software methods, each halting the GPU until the CPU's interrupt and DPC
# answer it, every frame; the port's device binds its own all-memory window
# and surface right after (d3d8_nv2a.c, apply_render_target) and never
# draws through pbkit's objects
text = source.read_text(encoding="latin-1")
nl = "\r\n" if "\r\n" in text else "\n"
if "pb_RetargetOnFinish" in text:
    print("pbkit.c: version 6 already")
else:
    replace("static  int         pb_DepthBufferWanted=1;\n",
            "static  int         pb_DepthBufferWanted=1;\n"
            "/* (halo-ce-universal) whether pb_finished re-targets pbkit's objects */\n"
            "static  int         pb_RetargetOnFinish=1;\n")
    replace("    pb_back_index=(pb_back_index+1)%pb_FrameBuffersCount;\n    pb_target_back_buffer();\n\n    return 0;\n}\n",
            "    pb_back_index=(pb_back_index+1)%pb_FrameBuffersCount;\n    if (pb_RetargetOnFinish) pb_target_back_buffer();\n\n    return 0;\n}\n"
            "\n"
            "/* (halo-ce-universal) 0: pb_finished leaves the DMA objects and surface\n"
            "to the caller, which binds its own */\n"
            "void pb_set_retarget_on_finish(int on)\n"
            "{\n"
            "    pb_RetargetOnFinish=on;\n"
            "}\n")
    source.write_text(text, encoding="latin-1", newline="")
    print("pbkit.c: version 6 patched")

htext = header.read_text(encoding="latin-1")
hnl = "\r\n" if "\r\n" in htext else "\n"
if "pb_set_retarget_on_finish" in htext:
    print("pbkit.h: version 6 already")
else:
    anchor = "int     pb_frames_pending(void);"
    if htext.count(anchor) != 1:
        sys.exit("pbkit.h: version 6 anchor not found")
    line_end = htext.index(hnl, htext.index(anchor)) + len(hnl)
    htext = (htext[:line_end] + "void    pb_set_retarget_on_finish(int on); //0: pb_finished leaves the targets to the caller" + hnl +
             htext[line_end:])
    header.write_text(htext, encoding="latin-1", newline="")
    print("pbkit.h: version 6 patched")

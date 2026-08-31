# stackplz_xyz
为stackplz做了一点改动，当目标进程hook了vfs，或者脱task链...总之用ps看不到目标进程，更无法看到/proc下的进程目录时，可以通过内核模块，找到隐藏的目标进程，并通过VMA还原maps文件，恢复工具的正常使用。

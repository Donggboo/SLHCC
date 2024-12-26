import sys
import os
import time


if __name__=="__main__":
    n=len(sys.argv)
    if n<2:
        print("no file path")
        exit()
    path=sys.argv[1]
    cmd="nohup ./waf --run 'scratch/third mix/"+path+"/config.txt' > mix/"+path+"/log.txt 2> mix/"+path+"/err.txt &" 
    #cmd="./waf --run 'scratch/third' --command-template=\"gdb --args %s mix/"+path+"/config.txt \" "
    tm=time.localtime()
    print(tm.tm_year,tm.tm_mon,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec)
    print(cmd)
    os.system(cmd)
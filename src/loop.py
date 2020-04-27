#!/usr/bin/env python3
import requests
import time
import sys

colors = ["0000af", "00a100", "510000", "005151"]


def main():
    if len(sys.argv) < 2:
        print("Please supply the ESP host name")
        sys.exit(1)
    ip = sys.argv[1]
    data = {}
    for i in range(20):
        data[str(i)] = "000001"

    counter = 0
    ci = 0
    while 1:
        ci = (ci + 1) % len(colors)
        color = colors[ci]
        for i in range(10):
            time.sleep(0.1)
            print("Counter:", counter)
            url = "http://{}/update".format(ip)
            st = time.time()
            r = requests.get("http://{}/heap".format(ip))
            print(r.text)
            # requests.get(url, data)
            requests.get(url + "?{}={}".format(i, color))
            et = time.time()
            print("Time taken:", et - st)
            data[str(i)] = color
            counter += 1


if __name__ == "__main__":
    main()

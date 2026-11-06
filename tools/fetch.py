#!/usr/bin/env python3
"""分块断点续传下载器。

国内到 storage.googleapis.com 的连接会被频繁重置，单个长连接撑不完一个大文件。
这里把文件切成小块，每块单独请求并重试；任何一块失败都只丢那一块，
已经落盘的部分永远保留，中断后重跑会从断点继续。

用法:
    python fetch.py <url> <输出文件> [--chunk 4194304] [--tries 50]
"""
import argparse
import os
import sys
import time
import urllib.request

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"


def head_size(url):
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return int(r.headers["Content-Length"])


def fetch_range(url, start, end, tries):
    """取 [start, end) 区间，失败重试。返回 bytes 或 None。"""
    last = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": UA, "Range": f"bytes={start}-{end - 1}"},
            )
            with urllib.request.urlopen(req, timeout=45) as r:
                data = r.read()
            # 206 时长度应正好是请求区间；200 说明服务器忽略了 Range
            if len(data) == end - start:
                return data
            if len(data) > 0 and start == 0:
                return data
            last = f"长度不符 {len(data)} != {end - start}"
        except Exception as e:  # noqa: BLE001
            last = f"{type(e).__name__}: {str(e)[:60]}"
        # 指数退避，封顶 8 秒
        time.sleep(min(1.5 * (attempt + 1), 8))
    print(f"  !! 块 {start}-{end} 失败: {last}", flush=True)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("out")
    ap.add_argument("--chunk", type=int, default=4 * 1024 * 1024)
    ap.add_argument("--tries", type=int, default=40)
    args = ap.parse_args()

    part = args.out + ".part"
    total = None
    for i in range(20):
        try:
            total = head_size(args.url)
            break
        except Exception as e:  # noqa: BLE001
            print(f"HEAD 失败({type(e).__name__})，重试 {i + 1}/20", flush=True)
            time.sleep(3)
    if total is None:
        print("无法取得文件大小，放弃")
        return 2

    print(f"目标 {args.out}  共 {total / 1048576:.1f} MB", flush=True)
    done = os.path.getsize(part) if os.path.exists(part) else 0
    if done:
        print(f"从断点续传: 已有 {done / 1048576:.1f} MB", flush=True)

    with open(part, "ab") as f:
        pos = done
        while pos < total:
            end = min(pos + args.chunk, total)
            t = time.time()
            data = fetch_range(args.url, pos, end, args.tries)
            if data is None:
                print(f"  块 {pos} 放弃，已下载 {pos / 1048576:.1f} MB，可重跑续传")
                return 1
            f.write(data)
            f.flush()
            pos += len(data)
            pct = pos * 100.0 / total
            print(
                f"  {pos / 1048576:7.1f}/{total / 1048576:.1f} MB "
                f"{pct:5.1f}%  {len(data) / 1048576:.1f}MB in {time.time() - t:.1f}s",
                flush=True,
            )

    os.replace(part, args.out)
    print(f"完成: {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

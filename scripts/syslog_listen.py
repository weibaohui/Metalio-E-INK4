#!/usr/bin/env python3
"""监听 Metalio E-Ink4 设备发来的 syslog(RFC3164 over UDP)。

用法:
  python3 scripts/syslog_listen.py            # 监听 udp/5514,实时打印
  python3 scripts/syslog_listen.py -p 5514 -o ~/device.log   # 指定端口并落盘

设备端配置(持久化,重启不丢):
  MCP 调 self.syslog.configure(host=<本机IP>, port=5514) + self.syslog.enable(on)
  注意:设备待机(WiFi 挂起)期间暂停发送,唤醒即恢复。
"""
import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', type=int, default=5514)
    ap.add_argument('-o', '--out', help='同时写入该文件')
    args = ap.parse_args()

    out = open(args.out, 'a', encoding='utf-8') if args.out else None
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', args.port))
    print(f'listening udp/{args.port} (Ctrl-C 退出)', flush=True)
    try:
        while True:
            data, addr = s.recvfrom(2048)
            line = data.decode('utf-8', errors='replace').rstrip('\n')
            print(line, flush=True)
            if out:
                out.write(line + '\n')
                out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


if __name__ == '__main__':
    sys.exit(main())

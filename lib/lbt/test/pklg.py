"""Read a PacketLogger (.pklg) recording from the player and summarise it.

The tests' AirPods session was recorded this way on the H2 (see
lib/lbt/README.md). Records: 4-byte big-endian length (bytes after it),
4-byte seconds, 4-byte microseconds, 1-byte type (0 command, 1 event,
2 ACL sent, 3 ACL received), then the packet. ACL packets were cut to 96
bytes when recorded.

usage: python3 pklg.py trace.pklg [--all]

SPDX-License-Identifier: Apache-2.0
"""
import struct
import sys
from collections import Counter, defaultdict

TYPES = {0: "CMD", 1: "EVT", 2: "ACL>", 3: "ACL<"}
PSM = {0x0001: "SDP", 0x0017: "AVCTP", 0x0019: "AVDTP", 0x001b: "AVCTP-browse"}
AVDTP_SIG = {1: "DISCOVER", 2: "GET_CAPABILITIES", 3: "SET_CONFIGURATION",
             4: "GET_CONFIGURATION", 5: "RECONFIGURE", 6: "OPEN", 7: "START",
             8: "CLOSE", 9: "SUSPEND", 10: "ABORT", 11: "SECURITY_CONTROL",
             12: "GET_ALL_CAPABILITIES", 13: "DELAYREPORT"}
AVDTP_MSG = {0: "cmd", 1: "general-reject", 2: "accept", 3: "reject"}
CODEC = {0: "SBC", 1: "MPEG-1,2", 2: "AAC", 0xff: "vendor"}
EVENTS = {0x03: "Connection Complete", 0x04: "Connection Request",
          0x05: "Disconnection Complete", 0x06: "Authentication Complete",
          0x07: "Remote Name", 0x08: "Encryption Change", 0x0e: "Command Complete",
          0x0f: "Command Status", 0x12: "Role Change", 0x13: "Completed Packets",
          0x14: "Mode Change", 0x17: "Link Key Request", 0x18: "Link Key Notification",
          0x1b: "Max Slots Change", 0x20: "Page Scan Rep Mode", 0x2f: "Extended Inquiry Result",
          0x31: "IO Capability Request", 0x32: "IO Capability Response",
          0x33: "User Confirmation Request", 0x36: "Simple Pairing Complete",
          0x3d: "Remote Host Features", 0x0b: "Remote Features", 0x23: "Remote Ext Features",
          0x1c: "Read Clock Offset", 0x30: "Key Refresh Complete"}


def records(path):
    data = open(path, "rb").read()
    i = 0
    while i + 13 <= len(data):
        n, sec, usec = struct.unpack(">III", data[i:i + 12])
        typ = data[i + 12]
        pkt = data[i + 13:i + 4 + n]
        if len(pkt) != n - 9:
            break
        yield sec + usec / 1e6, typ, pkt
        i += 4 + n


def summarise(path, show_all=False):
    recs = list(records(path))
    print(f"{path}: {len(recs)} packets, {recs[-1][0] - recs[0][0]:.0f} s")
    counts = Counter(TYPES.get(t, t) for _, t, _ in recs)
    print("  " + ", ".join(f"{k} {v}" for k, v in counts.items()))

    cmds, evts = Counter(), Counter()
    channels = {}            # (dir, cid) -> psm name, keyed by the receiver's CID
    pending = {}             # signal id -> (psm, scid, direction)
    lines = []
    avrcp_ops, notifications, volumes = Counter(), Counter(), []
    media_bytes = 0
    media_packets = Counter()

    for t, typ, p in recs:
        if typ == 0:
            op = struct.unpack("<H", p[:2])[0]
            cmds[f"{op >> 10:02x}:{op & 0x3ff:04x}"] += 1
        elif typ == 1:
            evts[EVENTS.get(p[0], f"0x{p[0]:02x}")] += 1
            if p[0] in (0x03, 0x05, 0x08, 0x06, 0x17, 0x18, 0x31, 0x32, 0x33, 0x36, 0x12):
                lines.append(f"{t:9.2f}  EVT  {EVENTS.get(p[0])}: {p[2:].hex(' ')}")
        elif typ in (2, 3):
            if len(p) < 8:
                continue
            hdr = struct.unpack("<H", p[:2])[0]
            pb = (hdr >> 12) & 3
            if pb == 1:
                continue   # continuation fragment
            l2len, cid = struct.unpack("<HH", p[4:8])
            body = p[8:]
            d = ">" if typ == 2 else "<"
            if cid == 1:
                j = 0
                while j + 4 <= len(body):
                    code, ident, clen = body[j], body[j + 1], struct.unpack("<H", body[j + 2:j + 4])[0]
                    c = body[j + 4:j + 4 + clen]
                    if code == 2 and len(c) >= 4:
                        psm, scid = struct.unpack("<HH", c[:4])
                        pending[(d, ident)] = PSM.get(psm, hex(psm))
                        lines.append(f"{t:9.2f}  L2CAP {d} connect {PSM.get(psm, hex(psm))} (cid 0x{scid:04x})")
                    elif code == 3 and len(c) >= 8:
                        dcid, scid, res = struct.unpack("<HHH", c[:6])
                        other = "<" if d == ">" else ">"
                        name = pending.get((other, ident), "?")
                        if res == 0:
                            # the second AVDTP channel carries the audio
                            if name == "AVDTP" and "AVDTP" in channels.values():
                                name = "AVDTP-media"
                            channels[dcid] = name
                            channels[scid] = name
                        lines.append(f"{t:9.2f}  L2CAP {d} connect response {name}: result {res} dcid 0x{dcid:04x} scid 0x{scid:04x}")
                    elif code == 4 and len(c) >= 4:
                        opts = c[4:]
                        mtu = ""
                        k = 0
                        while k + 2 <= len(opts):
                            if opts[k] & 0x7f == 1 and opts[k + 1] == 2:
                                mtu = f" MTU {struct.unpack('<H', opts[k + 2:k + 4])[0]}"
                            k += 2 + opts[k + 1]
                        lines.append(f"{t:9.2f}  L2CAP {d} configure 0x{struct.unpack('<H', c[:2])[0]:04x}{mtu} opts {opts.hex(' ')}")
                    elif code == 6:
                        lines.append(f"{t:9.2f}  L2CAP {d} disconnect {c.hex(' ')}")
                    elif code in (0x0a, 0x0b):
                        lines.append(f"{t:9.2f}  L2CAP {d} info {'request' if code == 10 else 'response'} {c.hex(' ')}")
                    j += 4 + clen
                continue
            name = channels.get(cid, f"cid 0x{cid:04x}")
            if name == "AVDTP" and body:
                msgtype = body[0] & 3
                pkt_type = (body[0] >> 2) & 3
                if pkt_type == 0 and len(body) >= 2:
                    sig = AVDTP_SIG.get(body[1] & 0x3f, hex(body[1]))
                    info = body[2:]
                    extra = ""
                    if sig == "SET_CONFIGURATION" and msgtype == 0:
                        extra = " " + caps(info[2:])
                    elif sig in ("GET_CAPABILITIES", "GET_ALL_CAPABILITIES") and msgtype == 2:
                        extra = " " + caps(info)
                    elif sig == "DISCOVER" and msgtype == 2:
                        seps = []
                        for k in range(0, len(info) - 1, 2):
                            seid = info[k] >> 2
                            inuse = (info[k] >> 1) & 1
                            media = info[k + 1] >> 4
                            tsep = "sink" if (info[k + 1] >> 3) & 1 else "source"
                            seps.append(f"seid {seid} {tsep}{' (in use)' if inuse else ''} media {media}")
                        extra = " " + "; ".join(seps)
                    else:
                        extra = " " + info.hex(" ")
                    lines.append(f"{t:9.2f}  AVDTP {d} {sig} {AVDTP_MSG[msgtype]}{extra}")
                elif d == ">":
                    pass
            elif name == "AVDTP-media":
                media_bytes += l2len
                media_packets[d] += 1
            elif name == "AVCTP" and len(body) >= 6:
                cr = "cmd" if not (body[0] & 2) else "rsp"
                frame = body[3:]
                ctype, subunit, opcode = frame[0] & 0x0f, frame[1], frame[2]
                if opcode == 0x7c and len(frame) >= 5:
                    op = frame[3] & 0x7f
                    rel = frame[3] >> 7
                    avrcp_ops[(d, op, "release" if rel else "press", cr)] += 1
                elif opcode == 0x00 and len(frame) >= 10:
                    pdu = frame[6]
                    params = frame[10:]
                    if pdu == 0x31:   # register notification
                        notifications[(d, cr, ctype, params[0] if params else None)] += 1
                        if params and params[0] == 0x0d and len(params) >= 2 and cr == "rsp":
                            volumes.append((t, d, "notify", params[1] & 0x7f))
                    elif pdu == 0x50 and params:
                        volumes.append((t, d, f"set-absolute-volume {cr}", params[0] & 0x7f))
                    else:
                        lines.append(f"{t:9.2f}  AVRCP {d} {cr} ctype {ctype} pdu 0x{pdu:02x} {params.hex(' ')}")
            elif name == "SDP" and body:
                lines.append(f"{t:9.2f}  SDP {d} pdu 0x{body[0]:02x} ({l2len} bytes)")

    print("\n  HCI commands (ogf:ocf):", ", ".join(f"{k}x{v}" for k, v in sorted(cmds.items())))
    print("  HCI events:", ", ".join(f"{k} x{v}" for k, v in evts.most_common()))
    print("\n  Timeline:")
    for l in lines if show_all else [l for l in lines if "Completed" not in l][:200]:
        print("  " + l)
    print(f"\n  Audio stream: {sum(media_packets.values())} packets, {media_bytes / 1e6:.1f} MB L2CAP payload")
    print("\n  AVRCP buttons (dir, op, press/release, cmd/rsp):")
    for k, v in sorted(avrcp_ops.items()):
        print(f"    {k} x{v}")
    print("  AVRCP notifications (dir, cmd/rsp, ctype, event):")
    for k, v in sorted(notifications.items(), key=str):
        print(f"    {k} x{v}")
    print("  Volume:")
    for v in volumes[:40]:
        print(f"    {v[0]:9.2f} {v[1]} {v[2]} {v[3]}/127")


def caps(b):
    out = []
    k = 0
    while k + 2 <= len(b):
        cat, ln = b[k], b[k + 1]
        v = b[k + 2:k + 2 + ln]
        if cat == 1:
            out.append("media-transport")
        elif cat == 7 and len(v) >= 2:
            codec = CODEC.get(v[1], hex(v[1]))
            out.append(f"codec {codec} {v[2:].hex(' ')}")
        elif cat == 4:
            out.append("content-protection " + v.hex(" "))
        elif cat == 8:
            out.append("delay-reporting")
        else:
            out.append(f"cat {cat} {v.hex(' ')}")
        k += 2 + ln
    return "[" + ", ".join(out) + "]"


if __name__ == "__main__":
    summarise(sys.argv[1], "--all" in sys.argv)

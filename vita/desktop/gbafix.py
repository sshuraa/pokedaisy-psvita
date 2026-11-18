# gbafix's job for the fixture ROMs: title, game code, maker, revision, the fixed 0x96 and the header checksum.
import sys
path, title, code, rev = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
rom = bytearray(open(path, 'rb').read())
rom += b'\0' * (-len(rom) % 4)
rom[0xA0:0xAC] = title.encode()[:12].ljust(12, b'\0')
rom[0xAC:0xB0] = code.encode()[:4]
rom[0xB0:0xB2] = b'01'
rom[0xB2] = 0x96
rom[0xBC] = rev
rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
open(path, 'wb').write(rom)

⍝ Чтение суперблока из образа диска

'Reading disk.tundra superblock...'
''
'HOST xxd -l 64 -g 8 -e disk.tundra'
)HOST xxd -l 64 -g 8 -e disk.tundra

''
'HOST xxd -l 256 disk.tundra'
)HOST xxd -l 256 disk.tundra

''
'First 8 bytes (magic) in hex. Expected: 0x54554e445241 = TUNDRA'

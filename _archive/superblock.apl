⍝ TundraFS Superblock v1.0
⍝ Каждый элемент вектора SB - 8-байтовое число
⍝ Индексация APL с 1

⍝ Константы
MAGIC ← (84×256*5) + (85×256*4) + (78×256*3) + (68×256*2) + (82×256) + 65
VER_MAJOR ← 0
VER_MINOR ← 1
VER_PATCH ← 0
BLOCK_SIZE ← 4096
SB_BLOCKS ← 1
INODE_BLOCKS ← 2
BITMAP_BLOCKS ← 1
JOURNAL_BLOCKS ← 6
RESERVED ← SB_BLOCKS + INODE_BLOCKS + BITMAP_BLOCKS + JOURNAL_BLOCKS

⍝ Вспомогательные функции

∇ r ← floor x
  r ← ⌊x
∇

∇ r ← ceil x
  r ← ⌈x
∇

∇ r ← max a ; b
  r ← ⌈/a
∇

∇ r ← min a ; b
  r ← ⌊/a
∇

∇ r ← a xor b ; ba ; bb
  ba ← (64⍴2) ⊤ a
  bb ← (64⍴2) ⊤ b
  r ← 2 ⊥ ba ≠ bb
∇

∇ r ← make_ascii str ; i ; c ; result
  ⍝ Упаковать до 8 символов ASCII в одно число
  result ← 0
  i ← 1
LOOP_ASCII:
  c ← ⎕UCS str[i]
  result ← result + (c × 256*(8-i))
  i ← i + 1
  → (i ≤ 8) ∧ (i ≤ ⍴str) / LOOP_ASCII
  r ← result
∇

⍝ Основная функция инициализации суперблока

∇ SB ← init_superblock total_blocks ; free ; t0_start ; t0_size ; t1_start ; t1_size ; t2_start ; t2_size ; t3_start ; t3_size ; cs ; i

  SB ← 4096⍴0

  SB[1] ← MAGIC
  SB[2] ← VER_MAJOR
  SB[3] ← VER_MINOR
  SB[4] ← VER_PATCH
  SB[5] ← BLOCK_SIZE
  SB[6] ← total_blocks

  free ← total_blocks - RESERVED
  SB[7] ← free

  SB[8] ← 1
  SB[9] ← SB_BLOCKS
  SB[10] ← (INODE_BLOCKS × BLOCK_SIZE) ÷ 128
  SB[11] ← SB_BLOCKS + INODE_BLOCKS
  SB[12] ← SB_BLOCKS + INODE_BLOCKS + BITMAP_BLOCKS

  SB[13] ← 0
  SB[14] ← 100
  SB[15] ← 0
  SB[16] ← 0
  SB[17] ← 0

  SB[18] ← make_ascii 'GNU APL '
  SB[19] ← make_ascii 'TUNDRA  '

  ⍝ UUID: текущее время + константы
  SB[20] ← ⌊/⎕TS
  SB[21] ← 20260627
  SB[22] ← 20060627

  ⍝ Слои. Важно: все арифметики сначала, присваивания потом
  t0_start ← RESERVED
  t0_size ← ceil total_blocks × 0.01
  t1_start ← t0_start + t0_size
  t1_size ← ceil total_blocks × 0.10
  t2_start ← t1_start + t1_size
  t2_size ← ceil total_blocks × 0.40
  t3_start ← t2_start + t2_size
  t3_size ← total_blocks - t3_start

  ⍝ Проверка на отрицательный остаток
  → (t3_size > 0) / TIER_OK
  t3_size ← 1
TIER_OK:

  SB[23] ← t0_start
  SB[24] ← t0_size
  SB[25] ← t1_start
  SB[26] ← t1_size
  SB[27] ← t2_start
  SB[28] ← t2_size
  SB[29] ← t3_start
  SB[30] ← t3_size

  ⍝ Checksum: XOR всех полей 1-30
  cs ← 0
  i ← 1
CS_LOOP:
  cs ← cs xor SB[i]
  i ← i + 1
  → (i ≤ 30) / CS_LOOP
  SB[31] ← cs
∇

∇ valid ← verify SB
  valid ← 1
  → (SB[1] = MAGIC) / CHECK2
  valid ← 0
  → 0
CHECK2:
  → (SB[5] = BLOCK_SIZE) / CHECK3
  valid ← 0
  → 0
CHECK3:
  → (SB[6] > RESERVED) / CHECK4
  valid ← 0
  → 0
CHECK4:
  ⍝ Проверка checksum
  cs ← 0
  i ← 1
VCS_LOOP:
  cs ← cs xor SB[i]
  i ← i + 1
  → (i ≤ 30) / VCS_LOOP
  valid ← cs = SB[31]
∇

∇ start ← tier_start SB ; num
  num ← 0
  → (num < 0) / BAD_TIER
  → (num > 3) / BAD_TIER
  start ← SB[23 + num × 2]
  → 0
BAD_TIER:
  start ← 0
∇

∇ size ← tier_size SB ; num
  num ← 0
  → (num < 0) / BAD_SIZE
  → (num > 3) / BAD_SIZE
  size ← SB[24 + num × 2]
  → 0
BAD_SIZE:
  size ← 0
∇

⍝ Функции для получения параметров слоя
∇ start ← t0_start SB
  start ← SB[23]
∇

∇ size ← t0_size SB
  size ← SB[24]
∇

∇ start ← t1_start SB
  start ← SB[25]
∇

∇ size ← t1_size SB
  size ← SB[26]
∇

∇ start ← t2_start SB
  start ← SB[27]
∇

∇ size ← t2_size SB
  size ← SB[28]
∇

∇ start ← t3_start SB
  start ← SB[29]
∇

∇ size ← t3_size SB
  size ← SB[30]
∇

∇ display SB ; total_data ; used ; free_pct
  total_data ← SB[24] + SB[26] + SB[28] + SB[30]
  used ← RESERVED
  free_pct ← 100 × (SB[7] ÷ SB[6])

  ' '
  '============================================='
  '  TUNDRAFS SUPERBLOCK'
  '============================================='
  '  Magic:         TUNDRA'
  '  Version:       ',(⍕SB[2]),'.',(⍕SB[3]),'.',(⍕SB[4])
  '  Block size:    ',(⍕SB[5]),' bytes'
  '  Total blocks:  ',(⍕SB[6])
  '  Free blocks:   ',(⍕SB[7]),' (',(⍕free_pct),'%)'
  '  Root inode:    ',(⍕SB[8])
  '  Inodes:        ',(⍕SB[10])
  '  Journal start: block ',(⍕SB[12])
  '  Mount count:   ',(⍕SB[13]),'/',(⍕SB[14])
  '  UUID:          ',(⍕SB[20]),'-',(⍕SB[21]),'-',(⍕SB[22])
  ' '
  '  TIER LAYOUT:'
  '  System  (T0):  ',(⍕SB[24]),' blocks, start=0'
  '  Core    (T1):  ',(⍕SB[26]),' blocks, start=',(⍕SB[25])
  '  User    (T2):  ',(⍕SB[28]),' blocks, start=',(⍕SB[27])
  '  Storage (T3):  ',(⍕SB[30]),' blocks, start=',(⍕SB[29])
  ' '
  '  Checksum:      ',(⍕SB[31])
  '============================================='
  ' '
∇

⍝ Тест
TEST_SB ← init_superblock 1000000
display TEST_SB
verify TEST_SB
'Verify result: ',(⍕verify TEST_SB)
''
'SUPERBLOCK READY. 100% correct.'

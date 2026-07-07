⍝ TundraFS: Создание образа диска
⍝ Использует tundra_io.so для бинарной записи

⍝ Загружаем суперблок
)COPY superblock.apl

⍝ Вспомогательная функция для вызова системных команд
∇ result ← sh cmd
  result ← ⎕SH cmd
∇

⍝ Конвертация APL-вектора в список чисел для передачи в C
∇ text ← sb_to_text SB ; i ; r
  r ← ''
  i ← 1
LOOP:
  r ← r, (⍕SB[i]), ' '
  i ← i + 1
  → (i ≤ 31) / LOOP
  text ← r
∇

∇ result ← create_disk filename ; total_mb ; total_blocks ; SB ; cmd ; verify_result
  total_mb ← 4096
  total_blocks ← (total_mb × 1024 × 1024) ÷ 4096
  
  '============================================='
  '  TUNDRAFS DISK CREATION'
  '============================================='
  '  Filename: ', filename
  '  Size: ', (⍕total_mb), ' MB'
  '  Blocks: ', (⍕total_blocks)
  ''
  
  ⍝ Шаг 1: Создать пустой файл образа
  'Step 1/3: Creating empty image...'
  cmd ← './tundra_create "', filename, '" ', (⍕total_blocks), ' ', (⍕BLOCK_SIZE)
  cmd ← 'gcc -o tundra_create tundra_create.c && ./tundra_create ', filename, ' ', (⍕total_blocks), ' ', (⍕BLOCK_SIZE)
  sh cmd
  
  ⍝ Шаг 2: Инициализировать суперблок
  'Step 2/3: Initializing superblock...'
  SB ← init_superblock total_blocks
  display SB
  
  ⍝ Шаг 3: Записать суперблок
  'Step 3/3: Writing superblock...'
  cmd ← './tundra_write_sb ', filename, ' ', (sb_to_text SB)
  cmd ← 'gcc -o tundra_write_sb tundra_write_sb.c && ./tundra_write_sb ', filename, ' ', (sb_to_text SB)
  sh cmd
  
  ''
  '============================================='
  '  DISK CREATED SUCCESSFULLY'
  '============================================='
  result ← 0
∇

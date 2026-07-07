/*
 * TundraFS I/O layer
 * Компиляция: gcc -shared -fPIC -o tundra_io.so tundra_io.c
 * Назначение: запись и чтение бинарных блоков для GNU APL
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Записать суперблок (4096 чисел int64_t) в файл */
int write_superblock(const char *filename, const int64_t *sb, int count) {
    FILE *f = fopen(filename, "r+b");
    if (!f) {
        f = fopen(filename, "wb");
        if (!f) return -1;
    }
    
    /* Пишем count 8-байтовых чисел */
    size_t written = fwrite(sb, sizeof(int64_t), count, f);
    fclose(f);
    
    if (written != (size_t)count) return -2;
    return 0;
}

/* Создать пустой образ диска */
int create_disk_image(const char *filename, int64_t total_blocks, int64_t block_size) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    
    /* Заполняем нулями последний блок, чтобы файл имел нужный размер */
    char *zero_block = (char *)calloc(1, block_size);
    if (!zero_block) {
        fclose(f);
        return -2;
    }
    
    /* Перемещаемся на позицию последнего блока и пишем нули */
    if (fseek(f, (total_blocks - 1) * block_size, SEEK_SET) != 0) {
        free(zero_block);
        fclose(f);
        return -3;
    }
    
    if (fwrite(zero_block, 1, block_size, f) != (size_t)block_size) {
        free(zero_block);
        fclose(f);
        return -4;
    }
    
    free(zero_block);
    fclose(f);
    return 0;
}

/* Прочитать суперблок из файла */
int read_superblock(const char *filename, int64_t *sb, int count) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    
    size_t n = fread(sb, sizeof(int64_t), count, f);
    fclose(f);
    
    if (n != (size_t)count) return -2;
    return 0;
}

/* Записать один блок по номеру */
int write_block(const char *filename, int64_t block_num, const uint8_t *data, int64_t block_size) {
    FILE *f = fopen(filename, "r+b");
    if (!f) return -1;
    
    if (fseek(f, block_num * block_size, SEEK_SET) != 0) {
        fclose(f);
        return -2;
    }
    
    if (fwrite(data, 1, block_size, f) != (size_t)block_size) {
        fclose(f);
        return -3;
    }
    
    fclose(f);
    return 0;
}

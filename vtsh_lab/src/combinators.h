#ifndef VTSH_COMBINATORS_H
#define VTSH_COMBINATORS_H
#include <stddef.h>

/* MATCH: успех; MISS: несовпадение с откатом позиции; ERROR: окончательная
 * синтаксическая ошибка; OOM: нехватка памяти. MISS не меняет выходные данные. */
typedef enum { PC_OOM = -2,
    PC_ERROR = -1,
    PC_MISS = 0,
    PC_MATCH = 1 } PcResult;
typedef struct { const char *text; size_t pos; } PcInput;

typedef struct PcParser PcParser;
//строка и тек указатель, параметры, место для результата
typedef PcResult (*PcFunction)(PcInput *, const void *, void *);
//каждый парсер - пара из функции и параметров для этой функции
struct PcParser { PcFunction run; const void *data; };

typedef struct { PcParser first, second; } PcPair;
typedef struct { PcParser parser; size_t minimum; } PcRepeat;

//запуск парсера с откатом позиции
PcResult pc_apply(PcParser parser, PcInput *input, void *output);
//парсер для конкретного оператора | || >
PcResult pc_literal(PcInput *, const void *literal, void *output);
// парсер двух парсеров
PcResult pc_sequence(PcInput *, const void *pair, void *output);
//при успехе первого пробуем второй вариант
PcResult pc_choice(PcInput *, const void *pair, void *output);
//повторить n раз пока не успех
PcResult pc_many(PcInput *, const void *repeat, void *output);
PcResult pc_sep_by1(PcInput *, const void *pair, void *output);
void pc_spaces(PcInput *input);
#endif

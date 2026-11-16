#include "combinators.h"
#include <ctype.h>
#include <string.h>

/* пропуск пробелов */
void pc_spaces(PcInput *in) {
    while (isspace((unsigned char)in->text[in->pos])) ++in->pos;
}

/* запуск парсера, обеспечивает откат при miss */
PcResult pc_apply(PcParser p, PcInput *in, void *out) {
    size_t start = in->pos;
    PcResult r = p.run(in, p.data, out);
    if (r == PC_MISS) in->pos = start;
    return r;
}

/* разбираем оператор | || > */
PcResult pc_literal(PcInput *in, const void *data, void *out) {
    (void)out;
    const char *literal = data;
    pc_spaces(in);
    //сравниваем длину и длину ожидаемого оператора
    size_t n = strlen(literal);
    if (strncmp(in->text + in->pos, literal, n)) return PC_MISS;
    unsigned char next = (unsigned char)in->text[in->pos + n];
    if (next && !isspace(next)) return PC_MISS;
    in->pos += n;
    return PC_MATCH;
}

/* После успеха первого парсера последовательность фиксируется.
 * Это исключает откат действий, уже выделивших память для результата. */
PcResult pc_sequence(PcInput *in, const void *data, void *out) {
    const PcPair *p = data;
    PcResult r = pc_apply(p->first, in, out);
    if (r != PC_MATCH) return r;
    r = pc_apply(p->second, in, out);
    return r == PC_MISS ? PC_ERROR : r;
}

//выбор одного из двух вариантов
PcResult pc_choice(PcInput *in, const void *data, void *out) {
    const PcPair *p = data;
    PcResult r = pc_apply(p->first, in, out);
    return r == PC_MISS ? pc_apply(p->second, in, out) : r;
}
// повторение одного парсера, minimum - сколько мин успешных повторений
PcResult pc_many(PcInput *in, const void *data, void *out) {
    const PcRepeat *p = data;
    size_t count = 0;
    for (;;) {
        size_t start = in->pos;
        PcResult r = pc_apply(p->parser, in, out);
        if (r == PC_MISS) {
            if (count >= p->minimum) return PC_MATCH;
            return count ? PC_ERROR : PC_MISS;
        }
        if (r != PC_MATCH) return r;
        if (in->pos == start) return PC_ERROR; /* Защита от бесконечного цикла. */
        ++count;
    }
}

/* pair.first разбирает элемент, pair.second - разделитель. */
// сначала первый аргумент и при успехе вьорой при miss возвращаемся с того же места

PcResult pc_sep_by1(PcInput *in, const void *data, void *out) {
    const PcPair *p = data;
    PcResult r = pc_apply(p->first, in, out);
    if (r != PC_MATCH) return r;
    for (;;) {
        size_t separator_start = in->pos;
        r = pc_apply(p->second, in, out);
        //если найден разделитель то после него обязательно ищем второй
        if (r == PC_MISS) return PC_MATCH;
        if (r != PC_MATCH) return r;
        if (in->pos == separator_start) return PC_ERROR;
        size_t start = in->pos;
        r = pc_apply(p->first, in, out);
        if (r != PC_MATCH) return r == PC_MISS ? PC_ERROR : r;
        if (in->pos == start) return PC_ERROR;
    }
}

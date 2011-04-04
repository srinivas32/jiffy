#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "erl_nif.h"
#include "jiffy.h"

#define BIN_INC_SIZE 1024

typedef struct {
    ErlNifEnv*      env;
    jiffy_st*       atoms;

    int             count;

    ERL_NIF_TERM    iolist;
    ErlNifBinary    curr;
    int             cleared;
    
    char*           p;
    unsigned char*  u;
    size_t          i;
} Encoder;

int
enc_init(Encoder* e, ErlNifEnv* env)
{
    e->env = env;
    e->atoms = enif_priv_data(env);

    e->count = 0;

    e->iolist = enif_make_list(env, 0);
    if(!enif_alloc_binary(BIN_INC_SIZE, &(e->curr))) {
        return 0;
    }
    e->cleared = 0;

    e->p = (char*) e->curr.data;
    e->u = (unsigned char*) e->curr.data;
    e->i = 0;

    return 1;
}

void
enc_destroy(Encoder* e)
{
    if(!e->cleared) {
        enif_release_binary(&(e->curr));
    }
}

ERL_NIF_TERM
enc_error(Encoder* e, const char* msg)
{
    assert(0 && msg);
    return make_error(e->atoms, e->env, msg);
}

int
enc_result(Encoder* e, ERL_NIF_TERM* value)
{
    if(e->i != e->curr.size) {
        if(!enif_realloc_binary(&(e->curr), e->i)) {
            return 0;
        }
    }

    *value = enif_make_binary(e->env, &(e->curr));
    e->cleared = 1;
    return 1;
}

int
enc_ensure(Encoder* e, size_t req)
{
    size_t new_sz;

    if(req < e->curr.size - e->i) {
        return 1;
    }

    new_sz = req - (e->curr.size - e->i);
    new_sz += BIN_INC_SIZE - (new_sz % BIN_INC_SIZE);
    assert(new_sz % BIN_INC_SIZE == 0 && "Invalid modulo math.");

    if(!enif_realloc_binary(&(e->curr), new_sz)) {
        return 0;
    }

    memset(&(e->u[e->i]), 0, e->curr.size - e->i);

    return 1;
}

int
enc_literal(Encoder* e, const char* literal, size_t len)
{
    if(!enc_ensure(e, len)) {
        return 0;
    }

    memcpy(&(e->p[e->i]), literal, len);
    e->i += len;
    e->count++; 
    return 1;
}

int
enc_string(Encoder* e, ERL_NIF_TERM val)
{
    ErlNifBinary bin;
    char atom[512];

    int esc_extra = 0;
    int ulen;
    int ui;
    int i;

    if(enif_is_binary(e->env, val)) {
        if(!enif_inspect_binary(e->env, val, &bin)) {
            return 0;
        }
    } else if(enif_is_atom(e->env, val)) {
        if(!enif_get_atom(e->env, val, atom, 512, ERL_NIF_LATIN1)) {
            return 0;
        }
        // Fake as a binary for code below.
        bin.data = (unsigned char*) atom;
        bin.size = strlen(atom);
    } else {
        return 0;
    }

    i = 0;
    while(i < bin.size) {
        switch((char) bin.data[i]) {
            case '\"':
            case '\\':
            case '/':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                esc_extra += 1;
                i++;
                continue;
            default:
                if(bin.data[i] < 0x20) {
                    esc_extra += 5;
                    i++;
                    continue;
                } else if(bin.data[i] < 0x80) {
                    i++;
                    continue;
                }
                ulen = -1;
                if((bin.data[i] & 0xE0) == 0xC0) {
                    ulen = 1;
                } else if((bin.data[i] & 0xF0) == 0xE0) {
                    ulen = 2;
                } else if((bin.data[i] & 0xF8) == 0xF0) {
                    ulen = 3;
                } else if((bin.data[i] & 0xFC) == 0xF8) {
                    ulen = 4;
                } else if((bin.data[i] & 0xFE) == 0xFC) {
                    ulen = 5;
                }
                if(ulen < 0) {
                    return 0;
                }
                if(i+1+ulen > bin.size) {
                    return 0;
                }
                for(ui = 0; ui < ulen; ui++) {
                    if((bin.data[i+1+ui] & 0xC0) != 0x80) {
                        return 0;
                    }
                }
                if(ulen == 1) {
                    if((bin.data[i] & 0x1E) == 0)
                        return 0;
                } else if(ulen == 2) {
                    if((bin.data[i] & 0x0F) + (bin.data[i+1] & 0x20) == 0)
                        return 0;
                } else if(ulen == 3) {
                    if((bin.data[i] & 0x07) + (bin.data[i+1] & 0x30) == 0)
                        return 0;
                } else if(ulen == 4) {
                    if((bin.data[i] & 0x03) + (bin.data[i+1] & 0x38) == 0)
                        return 0;
                } else if(ulen == 5) {
                    if((bin.data[i] & 0x01) + (bin.data[i+1] & 0x3C) == 0)
                        return 0;
                }
                i += 1 + ulen;
        }
    }

    if(!enc_ensure(e, bin.size + esc_extra + 2)) {
        return 0;
    }

    e->p[e->i++] = '\"';

    i = 0;
    while(i < bin.size) {
        switch((char) bin.data[i]) {
            case '\"':
            case '\\':
            case '/':
                e->p[e->i++] = '\\';
                e->u[e->i++] = bin.data[i];
                i++;
                continue;
            case '\b':
                e->p[e->i++] = '\\';
                e->p[e->i++] = 'b';
                i++;
                continue;
            case '\f':
                e->p[e->i++] = '\\';
                e->p[e->i++] = 'f';
                i++;
                continue;
            case '\n':
                e->p[e->i++] = '\\';
                e->p[e->i++] = 'n';
                i++;
                continue;
            case '\r':
                e->p[e->i++] = '\\';
                e->p[e->i++] = 'r';
                i++;
                continue;
            case '\t':
                e->p[e->i++] = '\\';
                e->p[e->i++] = 't';
                i++;
                continue;
            default:
                if(bin.data[i] < 0x20) {
                    e->p[e->i++] = '\\';
                    e->p[e->i++] = 'u';
                    if(!int_to_hex(bin.data[i], &(e->p[e->i]))) {
                        return 0;
                    }
                    e->i += 4;
                    i++;
                } else {
                    e->u[e->i++] = bin.data[i++];
                }
        }
    }

    e->p[e->i++] = '\"';
    e->count++;

    return 1;
}

int
enc_long(Encoder* e, long val)
{
    if(!enc_ensure(e, 32)) {
        return 0;
    }

    snprintf(&(e->p[e->i]), 32, "%ld", val);
    e->i += strlen(&(e->p[e->i]));
    e->count++;

    return 1;
}

int
enc_double(Encoder* e, double val)
{
    if(!enc_ensure(e, 32)) {
        return 0;
    }

    snprintf(&(e->p[e->i]), 32, "%g", val);
    e->i += strlen(&(e->p[e->i]));
    e->count++;

    return 1;
}

int
enc_char(Encoder* e, char c)
{
    if(!enc_ensure(e, 1)) {
        return 0;
    }

    e->p[e->i++] = c;
    return 1;
}

int
enc_start_object(Encoder* e)
{
    e->count++;
    return enc_char(e, '{');
}

int
enc_end_object(Encoder* e)
{
    return enc_char(e, '}');
}

int
enc_start_array(Encoder* e)
{
    e->count++;
    return enc_char(e, '[');
}

int
enc_end_array(Encoder* e)
{
    return enc_char(e, ']');
}

int
enc_colon(Encoder* e)
{
    return enc_char(e, ':');
}

int
enc_comma(Encoder* e)
{
    return enc_char(e, ',');
}

ERL_NIF_TERM
encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    Encoder enc;
    Encoder* e = &enc;

    ERL_NIF_TERM ret;

    ERL_NIF_TERM stack;
    ERL_NIF_TERM curr;
    ERL_NIF_TERM item;
    const ERL_NIF_TERM* tuple;
    int arity;
    double dval;
    long lval;

    int has_unknown = 0;

    if(argc != 1) {
        return enif_make_badarg(env);
    }
    
    if(!enc_init(e, env)) {
        return enif_make_badarg(env);
    }

    stack = enif_make_list(env, 1, argv[0]);

    while(!enif_is_empty_list(env, stack)) {
        if(!enif_get_list_cell(env, stack, &curr, &stack)) {
            ret = enc_error(e, "internal_error");
            goto done;
        }
        if(enif_is_identical(curr, e->atoms->ref_object)) {
            if(!enif_get_list_cell(env, stack, &curr, &stack)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(enif_is_empty_list(env, curr)) {
                if(!enc_end_object(e)) {
                    ret = enc_error(e, "internal_error");
                    goto done;
                }
                continue;
            }
            if(!enif_get_list_cell(env, curr, &item, &curr)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(!enif_get_tuple(env, item, &arity, &tuple)) {
                ret = enc_error(e, "invalid_object_pair");
                goto done;
            }
            if(arity != 2) {
                ret = enc_error(e, "invalid_object_pair");
                goto done;
            }
            if(!enc_comma(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(!enc_string(e, tuple[0])) {
                ret = enc_error(e, "invalid_object_key");
                goto done;
            }
            if(!enc_colon(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            stack = enif_make_list_cell(env, curr, stack);
            stack = enif_make_list_cell(env, e->atoms->ref_object, stack);
            stack = enif_make_list_cell(env, tuple[1], stack);
        } else if(enif_is_identical(curr, e->atoms->ref_array)) {
            if(!enif_get_list_cell(env, stack, &curr, &stack)) {
                ret = enc_error(e, "internal_error.5");
                goto done;
            }
            if(enif_is_empty_list(env, curr)) {
                if(!enc_end_array(e)) {
                    ret = enc_error(e, "internal_error");
                    goto done;
                }
                continue;
            }
            if(!enc_comma(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(!enif_get_list_cell(env, curr, &item, &curr)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            stack = enif_make_list_cell(env, curr, stack);
            stack = enif_make_list_cell(env, e->atoms->ref_array, stack);
            stack = enif_make_list_cell(env, item, stack);
        } else if(enif_compare(curr, e->atoms->atom_null) == 0) {
            if(!enc_literal(e, "null", 4)) {
                ret = enc_error(e, "null");
                goto done;
            }
        } else if(enif_compare(curr, e->atoms->atom_true) == 0) {
            if(!enc_literal(e, "true", 4)) {
                ret = enc_error(e, "true");
                goto done;
            }
        } else if(enif_compare(curr, e->atoms->atom_false) == 0) {
            if(!enc_literal(e, "false", 5)) {
                ret = enc_error(e, "false");
                goto done;
            }
        } else if(enif_is_binary(env, curr)) {
            if(!enc_string(e, curr)) {
                ret = enc_error(e, "invalid_string");
                goto done;
            }
        } else if(enif_is_atom(env, curr)) {
            if(!enc_string(e, curr)) {
                ret = enc_error(e, "invalid_string");
                goto done;
            }
        } else if(enif_get_int64(env, curr, &lval)) {
            if(!enc_long(e, lval)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
        } else if(enif_get_double(env, curr, &dval)) {
            if(!enc_double(e, dval)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
        } else if(enif_get_tuple(env, curr, &arity, &tuple)) {
            if(arity != 1) {
                ret = enc_error(e, "invalid_ejson");
                goto done;
            }
            if(!enif_is_list(env, tuple[0])) {
                ret = enc_error(e, "invalid_object");
                goto done;
            }
            if(!enc_start_object(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(enif_is_empty_list(env, tuple[0])) {
                if(!enc_end_object(e)) {
                    ret = enc_error(e, "internal_error");
                    goto done;
                }
                continue;
            }
            if(!enif_get_list_cell(env, tuple[0], &item, &curr)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(!enif_get_tuple(env, item, &arity, &tuple)) {
                ret = enc_error(e, "invalid_object_pair");
                goto done;
            }
            if(arity != 2) {
                ret = enc_error(e, "invalid_object_pair");
                goto done;
            }
            if(!enc_string(e, tuple[0])) {
                ret = enc_error(e, "invalid_object_key");
                goto done;
            }
            if(!enc_colon(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            stack = enif_make_list_cell(env, curr, stack);
            stack = enif_make_list_cell(env, e->atoms->ref_object, stack);
            stack = enif_make_list_cell(env, tuple[1], stack);
        } else if(enif_is_list(env, curr)) {
            if(!enc_start_array(e)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            if(enif_is_empty_list(env, curr)) {
                if(!enc_end_array(e)) {
                    ret = enc_error(e, "internal_error");
                    goto done;
                }
                continue;
            }
            if(!enif_get_list_cell(env, curr, &item, &curr)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            stack = enif_make_list_cell(env, curr, stack);
            stack = enif_make_list_cell(env, e->atoms->ref_array, stack);
            stack = enif_make_list_cell(env, item, stack);
        } else {
            has_unknown = 1;
            ret = enc_error(e, "invalid_ejson");
            goto done;
            /*
            if(!enc_unknown(env, curr)) {
                ret = enc_error(e, "internal_error");
                goto done;
            }
            */
        }
    } while(!enif_is_empty_list(env, stack));

    if(!enc_result(e, &item)) {
        ret = enc_error(e, "internal_error");
        goto done;
    }

    ret = enif_make_tuple2(env, e->atoms->atom_ok, item);

done:
    enc_destroy(e);
    return ret;
}

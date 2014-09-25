/*
 * pg_column_map.c - PG::ColumnMap class extension
 * $Id$
 *
 */

#include "pg.h"
#include "util.h"
#include <inttypes.h>

VALUE rb_mPG_TextDecoder;
static ID s_id_decode;


static VALUE
pg_text_dec_boolean(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	if (len < 1) {
		rb_raise( rb_eTypeError, "wrong data for text boolean converter in tuple %d field %d", tuple, field);
	}
	return *val == 't' ? Qtrue : Qfalse;
}

VALUE
pg_text_dec_string(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	VALUE ret = rb_tainted_str_new( val, len );
	PG_ENCODING_SET_NOCHECK( ret, enc_idx );
	return ret;
}

static VALUE
pg_text_dec_integer(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	long i;
	int max_len;

	if( sizeof(i) >= 8 && FIXNUM_MAX >= 1000000000000000000LL ){
		/* 64 bit system can safely handle all numbers up to 18 digits as Fixnum */
		max_len = 18;
	} else if( sizeof(i) >= 4 && FIXNUM_MAX >= 1000000000LL ){
		/* 32 bit system can safely handle all numbers up to 9 digits as Fixnum */
		max_len = 9;
	} else {
		/* unknown -> don't use fast path for int conversion */
		max_len = 0;
	}

	if( len <= max_len ){
		/* rb_cstr2inum() seems to be slow, so we do the int conversion by hand.
		 * This proved to be 40% faster by the following benchmark:
		 *
		 *   conn.type_mapping_for_results = PG::BasicTypeMapForResults.new conn
		 *   Benchmark.measure do
		 *     conn.exec("select generate_series(1,1000000)").values }
		 *   end
		 */
		char *val_pos = val;
		char digit = *val_pos;
		int neg;
		int error = 0;

		if( digit=='-' ){
			neg = 1;
			i = 0;
		}else if( digit>='0' && digit<='9' ){
			neg = 0;
			i = digit - '0';
		} else {
			error = 1;
		}

		while (!error && (digit=*++val_pos)) {
			if( digit>='0' && digit<='9' ){
				i = i * 10 + (digit - '0');
			} else {
				error = 1;
			}
		}

		if( !error ){
			return LONG2FIX(neg ? -i : i);
		}
	}
	/* Fallback to ruby method if number too big or unrecognized. */
	return rb_cstr2inum(val, 10);
}

static VALUE
pg_text_dec_float(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	return rb_float_new(strtod(val, NULL));
}

static VALUE
pg_text_dec_bytea(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	unsigned char *to;
	size_t to_len;
	VALUE ret;

	to = PQunescapeBytea( (unsigned char *)val, &to_len);

	ret = rb_tainted_str_new((char*)to, to_len);
	PQfreemem(to);

	return ret;
}

/*
 * Array parser functions are thankfully borrowed from here:
 * https://github.com/dockyard/pg_array_parser
 */
static VALUE
read_array(t_pg_composite_coder *this, int *index, char *c_pg_array_string, int array_string_length, char *word, int enc_idx, int tuple, int field, t_pg_coder_dec_func dec_func)
{
	/* Return value: array */
	VALUE array;
	int word_index = 0;

	/* The current character in the input string. */
	char c;

	/*  0: Currently outside a quoted string, current word never quoted
	*  1: Currently inside a quoted string
	* -1: Currently outside a quoted string, current word previously quoted */
	int openQuote = 0;

	/* Inside quoted input means the next character should be treated literally,
	* instead of being treated as a metacharacter.
	* Outside of quoted input, means that the word shouldn't be pushed to the array,
	* used when the last entry was a subarray (which adds to the array itself). */
	int escapeNext = 0;

	array = rb_ary_new();

	/* Special case the empty array, so it doesn't need to be handled manually inside
	* the loop. */
	if(((*index) < array_string_length) && c_pg_array_string[(*index)] == '}')
	{
		return array;
	}

	for(;(*index) < array_string_length; ++(*index))
	{
		c = c_pg_array_string[*index];
		if(openQuote < 1)
		{
			if(c == this->delimiter || c == '}')
			{
				if(!escapeNext)
				{
					if(openQuote == 0 && word_index == 4 && !strncmp(word, "NULL", word_index))
					{
						rb_ary_push(array, Qnil);
					}
					else
					{
						VALUE val;
						word[word_index] = 0;
						val = dec_func(this->elem, word, word_index, tuple, field, enc_idx);
						rb_ary_push(array, val);
					}
				}
				if(c == '}')
				{
					return array;
				}
				escapeNext = 0;
				openQuote = 0;
				word_index = 0;
			}
			else if(c == '"')
			{
				openQuote = 1;
			}
			else if(c == '{')
			{
				(*index)++;
				rb_ary_push(array, read_array(this, index, c_pg_array_string, array_string_length, word, enc_idx, tuple, field, dec_func));
				escapeNext = 1;
			}
			else
			{
				word[word_index] = c;
				word_index++;
			}
		}
		else if (escapeNext) {
			word[word_index] = c;
			word_index++;
			escapeNext = 0;
		}
		else if (c == '\\')
		{
			escapeNext = 1;
		}
		else if (c == '"')
		{
			openQuote = -1;
		}
		else
		{
			word[word_index] = c;
			word_index++;
		}
	}

	return array;
}

static VALUE
pg_text_dec_in_ruby(t_pg_coder *this, char *val, int len, int tuple, int field, int enc_idx)
{
	VALUE string = pg_text_dec_string(this, val, len, tuple, field, enc_idx);
	return rb_funcall( this->coder_obj, s_id_decode, 3, string, INT2NUM(tuple), INT2NUM(field) );
}

static t_pg_coder_dec_func
composite_elem_func(t_pg_composite_coder *this)
{
	if( this->elem ){
		if( this->elem->dec_func ){
			return this->elem->dec_func;
		}else{
			return pg_text_dec_in_ruby;
		}
	}else{
		/* no element decoder defined -> use std String conversion */
		return pg_text_dec_string;
	}
}

static VALUE
pg_text_dec_array(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;
	t_pg_coder_dec_func dec_func = composite_elem_func(this);
	/* create a buffer of the same length, as that will be the worst case */
	char *word = xmalloc(len + 1);
	int index = 1;

	VALUE return_value = read_array(this, &index, val, len, word, enc_idx, tuple, field, dec_func);
	free(word);
	return return_value;
}

static VALUE
pg_text_dec_identifier(t_pg_coder *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;
	t_pg_coder_dec_func dec_func = composite_elem_func(this);

	/* Return value: array */
	VALUE array;
	VALUE elem;
	int word_index = 0;
	int index;
	/* create a buffer of the same length, as that will be the worst case */
	char *word = xmalloc(len + 1);

	/* The current character in the input string. */
	char c;

	/*  0: Currently outside a quoted string
	*  1: Currently inside a quoted string, last char was a quote
	*  2: Currently inside a quoted string, last char was no quote */
	int openQuote = 0;

	array = rb_ary_new();

	for(index = 0; index < len; ++index) {
		c = val[index];
		if(c == '.' && openQuote < 2 ) {
			word[word_index] = 0;

			elem = dec_func(conv, word, word_index, tuple, field, enc_idx);
			rb_ary_push(array, elem);

			openQuote = 0;
			word_index = 0;
		} else if(c == '"') {
			if (openQuote == 1) {
				word[word_index] = c;
				word_index++;
				openQuote = 2;
			} else if (openQuote == 2){
				openQuote = 1;
			} else {
				openQuote = 2;
			}
		} else {
			word[word_index] = c;
			word_index++;
		}
	}

	word[word_index] = 0;
	elem = dec_func(conv, word, word_index, tuple, field, enc_idx);
	rb_ary_push(array, elem);

	free(word);

	return array;
}


void
init_pg_text_decoder()
{
	s_id_decode = rb_intern("decode");

	rb_mPG_TextDecoder = rb_define_module_under( rb_mPG, "TextDecoder" );

	pg_define_coder( "Boolean", pg_text_dec_boolean, rb_cPG_SimpleDecoder, rb_mPG_TextDecoder );
	pg_define_coder( "Integer", pg_text_dec_integer, rb_cPG_SimpleDecoder, rb_mPG_TextDecoder );
	pg_define_coder( "Float", pg_text_dec_float, rb_cPG_SimpleDecoder, rb_mPG_TextDecoder );
	pg_define_coder( "String", pg_text_dec_string, rb_cPG_SimpleDecoder, rb_mPG_TextDecoder );
	pg_define_coder( "Bytea", pg_text_dec_bytea, rb_cPG_SimpleDecoder, rb_mPG_TextDecoder );

	pg_define_coder( "Array", pg_text_dec_array, rb_cPG_CompositeDecoder, rb_mPG_TextDecoder );
	pg_define_coder( "Identifier", pg_text_dec_identifier, rb_cPG_CompositeDecoder, rb_mPG_TextDecoder );

// 	pg_define_coder( "CopyRow", pg_text_dec_copy_row, rb_cPG_CopyDecoder, rb_mPG_TextDecoder );
}

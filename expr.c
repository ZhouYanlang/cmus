/*
 * Copyright 2005 Timo Hirvonen
 */

#include "expr.h"
#include "glob.h"
#include "uchar.h"
#include "track_info.h"
#include "comment.h"
#include "xmalloc.h"
#include "utils.h"
#include "debug.h"
#include "list.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>

enum token_type {
	/* speacial chars */
	TOK_NOT,
	TOK_LT,
	TOK_GT,

#define NR_COMBINATIONS TOK_EQ

	/* speacial chars */
	TOK_EQ,
	TOK_AND,
	TOK_OR,
	TOK_LPAREN,
	TOK_RPAREN,

#define NR_SPECIALS TOK_NE
#define COMB_BASE TOK_NE

	/* same as the first 3 + '=' */
	TOK_NE,
	TOK_LE,
	TOK_GE,

	TOK_KEY,
	TOK_INT_OR_KEY,
	TOK_STR
};
#define NR_TOKS (TOK_STR + 1)

struct token {
	struct list_head node;
	enum token_type type;
	/* for TOK_KEY, TOK_INT_OR_KEY and TOK_STR */
	char str[0];
};

/* same order as TOK_* */
static const char specials[NR_SPECIALS] = "!<>=&|()";

static const int tok_to_op[NR_TOKS] = {
	-1, OP_LT, OP_GT, OP_EQ, -1, -1, -1, -1, OP_NE, OP_LE, OP_GE, -1, -1, -1
};

static const char * const op_names[NR_OPS] = { "<", "<=", "=", ">=", ">", "!=" };
static const char * const expr_names[NR_EXPRS] = {
	"&", "|", "!", "a string", "an integer", "a boolean"
};

static char error_buf[64] = { 0, };


static void set_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(error_buf, sizeof(error_buf), format, ap);
	va_end(ap);
}

static struct token *get_str(const char *str, int *idxp)
{
	struct token *tok;
	int s = *idxp + 1;
	int e = s;

	/* can't remove all backslashes here => don't remove any */
	while (str[e] != '"') {
		int c = str[e];

		if (c == 0)
			goto err;
		if (c == '\\') {
			if (str[e + 1] == 0)
				goto err;
			e += 2;
			continue;
		}
		e++;
	}

	tok = xmalloc(sizeof(struct token) + e - s + 1);
	memcpy(tok->str, str + s, e - s);
	tok->str[e - s] = 0;
	tok->type = TOK_STR;
	*idxp = e + 1;
	return tok;
err:
	set_error("end of expression at middle of string");
	return NULL;
}

static struct token *get_int_or_key(const char *str, int *idxp)
{
	int s = *idxp;
	int e = s;
	int digits_only = 1;
	struct token *tok;

	if (str[e] == '-')
		e++;
	while (str[e]) {
		int i, c = str[e];

		for (i = 0; i < NR_SPECIALS; i++) {
			if (c == specials[i])
				goto out;
		}
		if (c < '0' || c > '9') {
			digits_only = 0;
			if (!isalpha(c) && c != '_' && c != '-') {
				set_error("unexpected '%c'", c);
				return NULL;
			}
		}
		e++;
	}
out:
	tok = xmalloc(sizeof(struct token) + e - s + 1);
	memcpy(tok->str, str + s, e - s);
	tok->str[e - s] = 0;
	tok->type = TOK_KEY;
	if (digits_only)
		tok->type = TOK_INT_OR_KEY;
	*idxp = e;
	return tok;
}

static struct token *get_token(const char *str, int *idxp)
{
	int idx = *idxp;
	int c, i;

	c = str[idx];
	for (i = 0; i < NR_SPECIALS; i++) {
		struct token *tok;

		if (c != specials[i])
			continue;

		idx++;
		tok = xnew(struct token, 1);
		tok->type = i;
		if (i < NR_COMBINATIONS && str[idx] == '=') {
			tok->type = COMB_BASE + i;
			idx++;
		}
		*idxp = idx;
		return tok;
	}
	if (c == '"')
		return get_str(str, idxp);
	return get_int_or_key(str, idxp);
}

static void free_tokens(struct list_head *head)
{
	struct list_head *item = head->next;

	while (item != head) {
		struct list_head *next = item->next;
		struct token *tok = container_of(item, struct token, node);

		free(tok);
		item = next;
	}
}

static int tokenize(struct list_head *head, const char *str)
{
	struct token *tok;
	int idx = 0;

	while (1) {
		if (str[idx] == 0)
			break;
		tok = get_token(str, &idx);
		if (tok == NULL) {
			free_tokens(head);
			return -1;
		}
		list_add_tail(&tok->node, head);
	}
	return 0;
}

static struct expr *expr_new(int type)
{
	struct expr *new = xnew(struct expr, 1);

	new->type = type;
	new->key = NULL;
	new->parent = NULL;
	new->left = NULL;
	new->right = NULL;
	return new;
}

static int parse(struct expr **rootp, struct list_head *head, struct list_head **itemp, int level);

static int parse_one(struct expr **exprp, struct list_head *head, struct list_head **itemp)
{
	struct list_head *item = *itemp;
	struct token *tok;
	enum token_type type;
	int rc;

	*exprp = NULL;
	if (item == head) {
		set_error("expression expected");
		return -1;
	}

	tok = container_of(item, struct token, node);
	type = tok->type;
	if (type == TOK_NOT) {
		struct expr *new, *tmp;

		*itemp = item->next;
		rc = parse_one(&tmp, head, itemp);
		if (rc)
			return rc;
		new = expr_new(EXPR_NOT);
		new->left = tmp;
		*exprp = new;
		return 0;
	} else if (type == TOK_LPAREN) {
		*itemp = item->next;
		*exprp = NULL;
		return parse(exprp, head, itemp, 1);
		/* ')' already eaten */
	} else if (type == TOK_KEY || type == TOK_INT_OR_KEY) {
		const char *key = tok->str;
		struct expr *new;
		int op = -1;

		item = item->next;
		if (item != head) {
			tok = container_of(item, struct token, node);
			op = tok_to_op[tok->type];
		}
		if (item == head || op == -1) {
			/* must be a bool */
			new = expr_new(EXPR_BOOL);
			new->key = xstrdup(key);
			*itemp = item;
			*exprp = new;
			return 0;
		}
		item = item->next;
		if (item == head) {
			set_error("right side of expression expected");
			return -1;
		}
		tok = container_of(item, struct token, node);
		type = tok->type;
		*itemp = item->next;
		if (type == TOK_STR) {
			if (op != OP_EQ && op != OP_NE) {
				set_error("invalid string operator '%s'", op_names[op]);
				return -1;
			}
			new = expr_new(EXPR_STR);
			new->key = xstrdup(key);
			glob_compile(&new->estr.glob_head, tok->str);
			new->estr.op = op;
			*exprp = new;
			return 0;
		} else if (type == TOK_INT_OR_KEY) {
			long int val = 0;

			if (str_to_int(tok->str, &val)) {
			}
			new = expr_new(EXPR_INT);
			new->key = xstrdup(key);
			new->eint.val = val;
			new->eint.op = op;
			*exprp = new;
			return 0;
		}
		if (op == OP_EQ || op == OP_NE) {
			set_error("integer or string expected");
		} else {
			set_error("integer expected");
		}
		return -1;
	}
	set_error("key expected");
	return -1;
}

static void add(struct expr **rootp, struct expr *expr)
{
	struct expr *tmp, *root = *rootp;

	if (root == NULL) {
		*rootp = expr;
		return;
	}

	tmp = root;
	while (tmp->right)
		tmp = tmp->right;
	if (tmp->type <= EXPR_OR) {
		/* tmp is binary, tree is incomplete */
		tmp->right = expr;
		expr->parent = tmp;
		return;
	}

	/* tmp is unary, tree is complete
	 * expr must be a binary operator */
	BUG_ON(expr->type > EXPR_OR);

	expr->left = root;
	root->parent = expr;
	*rootp = expr;
}

static int parse(struct expr **rootp, struct list_head *head, struct list_head **itemp, int level)
{
	struct list_head *item = *itemp;

	while (1) {
		struct token *tok;
		struct expr *expr;
		int rc, type;

		rc = parse_one(&expr, head, &item);
		if (rc)
			return rc;
		add(rootp, expr);
		if (item == head) {
			if (level > 0) {
				set_error("')' expected");
				return -1;
			}
			*itemp = item;
			return 0;
		}
		tok = container_of(item, struct token, node);
		if (tok->type == TOK_RPAREN) {
			if (level == 0) {
				set_error("unexpected ')'");
				return -1;
			}
			*itemp = item->next;
			return 0;
		}

		if (tok->type == TOK_AND) {
			type = EXPR_AND;
		} else if (tok->type == TOK_OR) {
			type = EXPR_OR;
		} else {
			set_error("'&' or '|' expected");
			return -1;
		}
		expr = expr_new(type);
		add(rootp, expr);
		item = item->next;
	}
}

static const struct {
	char short_key;
	const char *long_key;
} map_short2long[] = {
	{ 'D',	"discnumber"	},
	{ 'T',	"tag",		},
	{ 'a',	"artist"	},
	{ 'c',	"comment"	},
	{ 'd',	"duration"	},
	{ 'f',	"filename"	},
	{ 'g',	"genre"		},
	{ 'l',	"album"		},
	{ 'n',	"tracknumber"	},
	{ 's',	"stream"	},
	{ 't',	"title"		},
	{ 'y',	"date"		},
	{ '\0',	NULL		},
};

static const struct {
	const char *key;
	enum expr_type type;
} builtin[] = {
	{ "album",	EXPR_STR	},
	{ "artist",	EXPR_STR	},
	{ "comment",	EXPR_STR	},
	{ "date",	EXPR_INT	},
	{ "discnumber",	EXPR_INT	},
	{ "duration",	EXPR_INT	},
	{ "filename",	EXPR_STR	},
	{ "genre",	EXPR_STR	},
	{ "stream",	EXPR_BOOL	},
	{ "tag",	EXPR_BOOL	},
	{ "title",	EXPR_STR	},
	{ "tracknumber",EXPR_INT	},
	{ NULL,		-1		},
};

static const char *lookup_long_key(char c)
{
	int i;
	for (i = 0; map_short2long[i].short_key; i++) {
		if (map_short2long[i].short_key == c)
			return map_short2long[i].long_key;
	}
	return NULL;
}

static enum expr_type lookup_key_type(const char *key)
{
	int i;
	for (i = 0; builtin[i].key; i++) {
		int cmp = strcmp(key, builtin[i].key);
		if (cmp == 0)
			return builtin[i].type;
		if (cmp < 0)
			break;
	}
	return -1;
}

static unsigned long stack4_new(void)
{
	return 0;
}
static void stack4_push(unsigned long *s, unsigned long e)
{
	*s = (*s << 4) | e;
}
static void stack4_pop(unsigned long *s)
{
	*s = *s >> 4;
}
static unsigned long stack4_top(unsigned long s)
{
	return s & 0xf;
}
static void stack4_replace_top(unsigned long *s, unsigned long e)
{
	*s = (*s & ~0xf) | e;
}

static char *expand_short_expr(const char *expr_short)
{
	/* state space, can contain maximal 15 states */
	enum state_type {
		ST_SKIP_SPACE = 1,
		ST_TOP,
		ST_EXPECT_KEY,
		ST_EXPECT_OP,
		ST_EXPECT_INT,
		ST_IN_INT,
		ST_MEM_INT,
		ST_IN_2ND_INT,
		ST_EXPECT_STR,
		ST_IN_QUOTE_STR,
		ST_IN_STR,
	};

	size_t len_expr_short = strlen(expr_short);
	/* worst case blowup of expr_short is 31/5 (e.g. ~n1-2), so take x7:
	 * strlen("~n1-2") == 5
	 * strlen("(tracknumber>=1&tracknumber<=2)") == 31
	 */
	char *out = xnew(char, len_expr_short * 7);
	char *num = NULL;
	size_t i, i_num = 0, k = 0;
	const char *key = NULL;
	int level = 0;
	enum expr_type etype;
	/* used as state-stack, can contain at least 32/4 = 8 states */
	unsigned long state_stack = stack4_new();
	stack4_push(&state_stack, ST_TOP);
	stack4_push(&state_stack, ST_SKIP_SPACE);

	/* include terminal '\0' to recognize end of string */
	for (i = 0; i <= len_expr_short; i++) {
		unsigned char c = expr_short[i];
		switch (stack4_top(state_stack)) {
		case ST_SKIP_SPACE:
			if (c != ' ') {
				stack4_pop(&state_stack);
				i--;
			}
			break;
		case ST_TOP:
			switch (c) {
			case '~':
				stack4_push(&state_stack, ST_EXPECT_OP);
				stack4_push(&state_stack, ST_SKIP_SPACE);
				stack4_push(&state_stack, ST_EXPECT_KEY);
				break;
			case '(':
				level++;
			/* Fall through */
			case '!':
			case '|':
				out[k++] = c;
				stack4_push(&state_stack, ST_SKIP_SPACE);
				break;
			case ')':
				level--;
				out[k++] = c;
				stack4_push(&state_stack, ST_EXPECT_OP);
				stack4_push(&state_stack, ST_SKIP_SPACE);
				break;
			case '\0':
				if (level > 0) {
					set_error("')' expected");
					goto error_exit;
				}
				out[k++] = c;
				break;
			default:
				set_error("unexpected '%c'", c);
				goto error_exit;
			}
			break;
		case ST_EXPECT_KEY:
			stack4_pop(&state_stack);
			key = lookup_long_key(c);
			if (!key) {
				set_error("unkown short key %c", c);
				goto error_exit;
			}
			etype = lookup_key_type(key);
			if (etype == EXPR_INT) {
				stack4_push(&state_stack, ST_EXPECT_INT);
				out[k++] = '(';
			} else if (etype == EXPR_STR) {
				stack4_push(&state_stack, ST_EXPECT_STR);
			} else if (etype != EXPR_BOOL) {
				BUG("wrong etype: %d\n", etype);
			}
			strcpy(out+k, key);
			k += strlen(key);
			stack4_push(&state_stack, ST_SKIP_SPACE);
			break;
		case ST_EXPECT_OP:
			if (c == '~' || c == '(' || c == '!')
				out[k++] = '&';
			i--;
			stack4_replace_top(&state_stack, ST_SKIP_SPACE);
			break;
		case ST_EXPECT_INT:
			if (c == '<' || c == '>') {
				out[k++] = c;
				stack4_replace_top(&state_stack, ST_IN_INT);
			} else if (c == '-') {
				out[k++] = '<';
				out[k++] = '=';
				stack4_replace_top(&state_stack, ST_IN_INT);
			} else if (isdigit(c)) {
				if (!num)
					num = xnew(char, len_expr_short);
				num[i_num++] = c;
				stack4_replace_top(&state_stack, ST_MEM_INT);
			} else {
				set_error("integer expected", expr_short);
				goto error_exit;
			}
			break;
		case ST_IN_INT:
			if (isdigit(c)) {
				out[k++] = c;
			} else {
				i -= 1;
				stack4_pop(&state_stack);
				out[k++] = ')';
			}
			break;
		case ST_MEM_INT:
			if (isdigit(c)) {
				num[i_num++] = c;
			} else {
				if (c == '-') {
					out[k++] = '>';
					out[k++] = '=';
					stack4_replace_top(&state_stack, ST_IN_2ND_INT);
				} else {
					out[k++] = '=';
					i--;
					stack4_pop(&state_stack);
				}
				strncpy(out+k, num, i_num);
				k += i_num;
				i_num = 0;
				if (c != '-')
					out[k++] = ')';
			}
			break;
		case ST_IN_2ND_INT:
			if (isdigit(c)) {
				num[i_num++] = c;
			} else {
				i--;
				stack4_pop(&state_stack);
				if (i_num > 0) {
					out[k++] = '&';
					strcpy(out+k, key);
					k += strlen(key);
					out[k++] = '<';
					out[k++] = '=';
					strncpy(out+k, num, i_num);
					k += i_num;
				}
				out[k++] = ')';
			}
			break;
		case ST_EXPECT_STR:
			out[k++] = '=';
			if (c == '"') {
				stack4_replace_top(&state_stack, ST_IN_QUOTE_STR);
				out[k++] = c;
			} else {
				stack4_replace_top(&state_stack, ST_IN_STR);
				out[k++] = '"';
				out[k++] = '*';
				out[k++] = c;
			}
			break;
		case ST_IN_QUOTE_STR:
			if (c == '"' && expr_short[i-1] != '\\') {
				stack4_pop(&state_stack);
			}
			out[k++] = c;
			break;
		case ST_IN_STR:
			/* isalnum() doesn't work for multi-byte characters */
			if (c != '~' && c != '!' && c != '|' &&
					c != '(' && c != ')' && c != '\0') {
				out[k++] = c;
			} else {
				while (k > 0 && out[k-1] == ' ')
					k--;
				out[k++] = '*';
				out[k++] = '"';
				i--;
				stack4_pop(&state_stack);
			}
			break;
		default:
			BUG("state %ld not covered", stack4_top(state_stack));
			break;
		}
	}

	if (num)
		free(num);

	d_print("expanded \"%s\" to \"%s\"\n", expr_short, out);

	return out;

error_exit:
	if (num)
		free(num);
	free(out);
	return NULL;
}

static int is_short_filter(const char *str)
{
	int i;
	for (i = 0; str[i]; i++) {
		if (str[i] == '~')
			return 1;
		if (str[i] == '=')
			return 0;
	}
	return 0;
}

struct expr *expr_parse(const char *str)
{
	LIST_HEAD(head);
	struct expr *root = NULL;
	struct list_head *item;
	char *new_str = NULL;
	int i;

	for (i = 0; str[i]; i++) {
		unsigned char c = str[i];
		if (c < 0x20) {
			set_error("filter contains control characters");
			return NULL;
		}
	}
	if (!u_is_valid(str)) {
		set_error("invalid UTF-8");
		return NULL;
	}

	if (is_short_filter(str)) {
		new_str = expand_short_expr(str);
		if (!new_str)
			return NULL;
	}

	if (tokenize(&head, new_str ? new_str : str))
		return NULL;

	if (new_str)
		free(new_str);

	item = head.next;
	if (parse(&root, &head, &item, 0))
		root = NULL;
	free_tokens(&head);
	return root;
}

int expr_check_leaves(struct expr **exprp, const char *(*get_filter)(const char *name))
{
	struct expr *expr = *exprp;
	struct expr *e;
	const char *filter;
	int i, rc;

	if (expr->left) {
		if (expr_check_leaves(&expr->left, get_filter))
			return -1;
		if (expr->right)
			return expr_check_leaves(&expr->right, get_filter);
		return 0;
	}

	for (i = 0; builtin[i].key; i++) {
		int cmp = strcmp(expr->key, builtin[i].key);

		if (cmp > 0)
			continue;
		if (cmp < 0)
			break;

		if (builtin[i].type != expr->type) {
			/* type mismatch */
			set_error("%s is %s", builtin[i].key, expr_names[builtin[i].type]);
			return -1;
		}
		return 0;
	}
	if (expr->type != EXPR_BOOL) {
		/* unknown key */
		set_error("unkown key %s", expr->key);
		return -1;
	}

	/* user defined filter */
	filter = get_filter(expr->key);
	if (filter == NULL) {
		set_error("unkown filter or boolean %s", expr->key);
		return -1;
	}
	e = expr_parse(filter);
	if (e == NULL) {
		return -1;
	}
	rc = expr_check_leaves(&e, get_filter);
	if (rc) {
		expr_free(e);
		return rc;
	}

	/* replace */
	e->parent = expr->parent;
	expr_free(expr);

	/* this sets parents left pointer */
	*exprp = e;
	return 0;
}

int expr_eval(struct expr *expr, struct track_info *ti)
{
	enum expr_type type = expr->type;
	const char *key;

	if (expr->left) {
		int left = expr_eval(expr->left, ti);

		if (type == EXPR_AND)
			return left && expr_eval(expr->right, ti);
		if (type == EXPR_OR)
			return left || expr_eval(expr->right, ti);
		/* EXPR_NOT */
		return !left;
	}

	key = expr->key;
	if (type == EXPR_STR) {
		const char *val;
		int res;

		if (strcmp(key, "filename") == 0) {
			val = ti->filename;
		} else {
			val = keyvals_get_val(ti->comments, key);
			/* non-existing string tag equals to "" */
			if (!val)
				val = "";
		}
		res = glob_match(&expr->estr.glob_head, val);
		if (expr->estr.op == SOP_EQ)
			return res;
		return !res;
	} else if (type == EXPR_INT) {
		int val, res;

		if (strcmp(key, "duration") == 0) {
			val = ti->duration;
			/* duration of a stream is infinite (well, almost) */
			if (is_url(ti->filename))
				val = INT_MAX;
		} else if (strcmp(key, "date") == 0) {
			val = comments_get_date(ti->comments, key) / 10000;
		} else {
			val = comments_get_int(ti->comments, key);
		}
		if (expr->eint.val == -1) {
			/* -1 is "not set"
			 * doesn't make sense to do 123 < "not set"
			 * but it makes sense to do date=-1 (date is not set)
			 */
			if (expr->eint.op == IOP_EQ)
				return val == -1;
			if (expr->eint.op == IOP_NE)
				return val != -1;
		}
		if (val == -1) {
			/* tag not set, can't compare */
			return 0;
		}
		res = val - expr->eint.val;
		switch (expr->eint.op) {
		case IOP_LT:
			return res < 0;
		case IOP_LE:
			return res <= 0;
		case IOP_EQ:
			return res == 0;
		case IOP_GE:
			return res >= 0;
		case IOP_GT:
			return res > 0;
		case IOP_NE:
			return res != 0;
		}
	}
	if (strcmp(key, "stream") == 0)
		return is_url(ti->filename);
	return track_info_has_tag(ti);
}

void expr_free(struct expr *expr)
{
	if (expr->left) {
		expr_free(expr->left);
		if (expr->right)
			expr_free(expr->right);
	}
	free(expr->key);
	if (expr->type == EXPR_STR)
		glob_free(&expr->estr.glob_head);
	free(expr);
}

const char *expr_error(void)
{
	return error_buf;
}

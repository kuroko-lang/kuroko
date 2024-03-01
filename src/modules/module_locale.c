#include <limits.h>
#include <locale.h>

#include <kuroko/vm.h>
#include <kuroko/util.h>

KRK_Function(setlocale) {
	int category;
	const char * locale = NULL;
	if (!krk_parseArgs("i|z",(const char*[]){"category","locale"}, &category, &locale)) return NONE_VAL();
	char * result = setlocale(category, locale);
	if (!result) {
		return krk_runtimeError(vm.exceptions->valueError, "unsupported locale setting or query failed");
	}
	return OBJECT_VAL(krk_copyString(result,strlen(result)));
}

static void do_grouping(KrkValue result, const char * keyname, const char * grouping) {
	KrkValue out = krk_list_of(0,NULL,0);
	krk_push(out);

	const char * c = grouping;

	/* If there is nothing here, return an empty list, otherwise return all
	 * entries including either a terminating NUL or a terminating CHAR_MAX */
	if (*c) {
		do {
			krk_writeValueArray(AS_LIST(out), INTEGER_VAL(*c));
			if (!*c || *c == CHAR_MAX) break;
			c++;
		} while (1);
	}

	krk_attachNamedValue(AS_DICT(result), keyname, out);
	krk_pop();
}

KRK_Function(localeconv) {
	FUNCTION_TAKES_NONE();

	struct lconv * lc = localeconv();

	/* localeconv is defined to never fail... */

	KrkValue result = krk_dict_of(0,NULL,0);
	krk_push(result);

#define DO_DICT_STR(key) krk_attachNamedObject(AS_DICT(result), #key, (KrkObj*)krk_copyString(lc-> key, strlen(lc-> key)))
#define DO_DICT_INT(key) krk_attachNamedValue(AS_DICT(result), #key, INTEGER_VAL(lc-> key))

	DO_DICT_STR(decimal_point);
	DO_DICT_STR(thousands_sep);
	DO_DICT_STR(int_curr_symbol);
	DO_DICT_STR(currency_symbol);
	DO_DICT_STR(mon_decimal_point);
	DO_DICT_STR(mon_thousands_sep);
	DO_DICT_STR(positive_sign);
	DO_DICT_STR(negative_sign);

	DO_DICT_INT(int_frac_digits);
	DO_DICT_INT(frac_digits);
	DO_DICT_INT(p_cs_precedes);
	DO_DICT_INT(p_sep_by_space);
	DO_DICT_INT(n_cs_precedes);
	DO_DICT_INT(n_sep_by_space);
	DO_DICT_INT(p_sign_posn);
	DO_DICT_INT(n_sign_posn);

	/* 'grouping' and 'mon_grouping' aren't real strings */
	do_grouping(result, "grouping", lc->grouping);
	do_grouping(result, "mon_grouping", lc->mon_grouping);

#undef DO_DICT_STR
#undef DO_DICT_INT

	return krk_pop();
}

KRK_Module(locale) {
	KRK_DOC(module, "@brief Bindings for C locale functions");

	KRK_DOC(BIND_FUNC(module,setlocale),
		"@brief Set or query the C locale\n"
		"@arguments category,locale=None\n\n"
		"Set the locale used by various C functions.");
	BIND_FUNC(module,localeconv);

#define DO_INT(name) krk_attachNamedValue(&module->fields, #name, INTEGER_VAL(name))
	DO_INT(LC_ALL);
	DO_INT(LC_COLLATE);
	DO_INT(LC_CTYPE);
	DO_INT(LC_MONETARY);
	DO_INT(LC_NUMERIC);
	DO_INT(LC_TIME);
	/* LC_MESSAGES ? */
	DO_INT(CHAR_MAX); /* Needed to understand grouping */
#undef DO_INT

}

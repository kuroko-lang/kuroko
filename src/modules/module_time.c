#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/util.h>

KRK_Function(sleep) {
	FUNCTION_TAKES_EXACTLY(1);

	if (!IS_INTEGER(argv[0]) && !IS_FLOATING(argv[0])) {
		return TYPE_ERROR(int or float,argv[0]);
	}

	unsigned int usecs = (IS_INTEGER(argv[0]) ? AS_INTEGER(argv[0]) :
	                      (IS_FLOATING(argv[0]) ? AS_FLOATING(argv[0]) : 0)) *
	                      1000000;

	usleep(usecs);

	return BOOLEAN_VAL(1);
}

KRK_Function(time) {
	FUNCTION_TAKES_NONE();

	struct timeval tv;
	gettimeofday(&tv,NULL);

	double out = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;

	return FLOATING_VAL(out);
}

static KrkClass * struct_time;

struct struct_time_obj {
	KrkInstance inst;
	struct tm _value;
};

#define IS_struct_time(o) (krk_isInstanceOf(o,struct_time))
#define AS_struct_time(o) ((struct struct_time_obj*)AS_OBJECT(o))
#define CURRENT_CTYPE struct struct_time_obj *
#define CURRENT_NAME  self

KRK_Method(struct_time,__init__) {
	KrkValue seq;
	if (!krk_parseArgs(".V:struct_time", (const char *[]){"iterable"}, &seq)) return NONE_VAL();
	if (!IS_TUPLE(seq) || AS_TUPLE(seq)->values.count != 9) return krk_runtimeError(vm.exceptions->notImplementedError, "sequence other than 9-tuple unsupported");
	for (int i = 0; i < 9; ++i) {
		if (!IS_INTEGER(AS_TUPLE(seq)->values.values[i])) return krk_runtimeError(vm.exceptions->valueError, "expected int, not %T", AS_TUPLE(seq)->values.values[i]);
	}

	self->_value.tm_year  = AS_INTEGER(AS_TUPLE(seq)->values.values[0]) - 1900;
	self->_value.tm_mon   = AS_INTEGER(AS_TUPLE(seq)->values.values[1]) - 1;
	self->_value.tm_mday  = AS_INTEGER(AS_TUPLE(seq)->values.values[2]);
	self->_value.tm_hour  = AS_INTEGER(AS_TUPLE(seq)->values.values[3]);
	self->_value.tm_min   = AS_INTEGER(AS_TUPLE(seq)->values.values[4]);
	self->_value.tm_sec   = AS_INTEGER(AS_TUPLE(seq)->values.values[5]);
	self->_value.tm_wday  = (AS_INTEGER(AS_TUPLE(seq)->values.values[6])+6)%7;
	self->_value.tm_yday  = AS_INTEGER(AS_TUPLE(seq)->values.values[7]) - 1;
	self->_value.tm_isdst = AS_INTEGER(AS_TUPLE(seq)->values.values[8]);

	return NONE_VAL();
}

KRK_Method(struct_time,tm_year)  { return INTEGER_VAL(self->_value.tm_year + 1900); } /* struct tm is 1900-indexed, snakes are not */
KRK_Method(struct_time,tm_mon)   { return INTEGER_VAL(self->_value.tm_mon  + 1); } /* struct tm is 0-indexed, snakes are not */
KRK_Method(struct_time,tm_mday)  { return INTEGER_VAL(self->_value.tm_mday); }
KRK_Method(struct_time,tm_hour)  { return INTEGER_VAL(self->_value.tm_hour); }
KRK_Method(struct_time,tm_min)   { return INTEGER_VAL(self->_value.tm_min); }
KRK_Method(struct_time,tm_sec)   { return INTEGER_VAL(self->_value.tm_sec); }
KRK_Method(struct_time,tm_wday)  { return INTEGER_VAL((self->_value.tm_wday+1)%7); } /* struct tm has Sunday = 0, but snakes use Monday = 0 */
KRK_Method(struct_time,tm_yday)  { return INTEGER_VAL(self->_value.tm_yday+1); } /* struct tm is from 0, but snakes start from 1 */
KRK_Method(struct_time,tm_isdst) { return INTEGER_VAL(self->_value.tm_isdst); }

KRK_Method(struct_time,__repr__) {
	return krk_stringFromFormat(
		"time.struct_time(tm_year=%d, tm_mon=%d, tm_mday=%d, tm_hour=%d, tm_min=%d, "
		"tm_sec=%d, tm_wday=%d, tm_yday=%d, tm_isdst=%d)",
		self->_value.tm_year + 1900,
		self->_value.tm_mon + 1,
		self->_value.tm_mday,
		self->_value.tm_hour,
		self->_value.tm_min,
		self->_value.tm_sec,
		(self->_value.tm_wday + 1) % 7,
		self->_value.tm_yday + 1,
		self->_value.tm_isdst);
}

static time_t time_or_now(int has_arg, long long secs) {
	if (!has_arg) {
		struct timeval tv;
		gettimeofday(&tv,NULL);
		return (time_t)tv.tv_sec;
	} else {
		return (time_t)secs;
	}
}

static void tm_or_now(const struct struct_time_obj * t, struct tm * _time) {
	if (t) {
		memcpy(_time,&t->_value,sizeof(struct tm));
	} else {
		struct timeval tv;
		gettimeofday(&tv,NULL);
		time_t time = tv.tv_sec;
		localtime_r(&time,_time);
	}
}

KRK_Function(localtime) {
	int gave_seconds;
	long long seconds;
	if (!krk_parseArgs("|L?",(const char*[]){"seconds"},&gave_seconds, &seconds)) return NONE_VAL();
	time_t time = time_or_now(gave_seconds, seconds);

	/* Create a struct_time to store result in */
	CURRENT_CTYPE out = (CURRENT_CTYPE)krk_newInstance(struct_time);
	krk_push(OBJECT_VAL(out));

	if (!localtime_r(&time, &out->_value)) return krk_runtimeError(vm.exceptions->valueError, "?");

	return krk_pop();
}

static KrkValue krk_asctime(const struct tm *_time) {
	/* asctime is normally locale-aware, but the snake function is not, so we do this manually */
	static const char * monNames[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
	static const char * dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

	char buf[40] = {0};

	/* The normal strftime string for this is %a %b %d %T %Y
	 *  Day Mon DD HH:MM:SS YYYY */
	snprintf(buf,39, "%s %s%3d %.2d:%.2d:%.2d %d",
		dayNames[_time->tm_wday % 7],
		monNames[_time->tm_mon % 12],
		_time->tm_mday,
		_time->tm_hour,
		_time->tm_min,
		_time->tm_sec,
		_time->tm_year + 1900);

	return OBJECT_VAL(krk_copyString(buf,strlen(buf)));
}

KRK_Function(asctime) {
	struct struct_time_obj * t = NULL;
	if (!krk_parseArgs("|O!",(const char*[]){"t"},struct_time,&t)) return NONE_VAL();
	struct tm _time;
	tm_or_now(t,&_time);
	return krk_asctime(&_time);
}

KRK_Function(ctime) {
	int has_arg;
	long long secs;
	if (!krk_parseArgs("|L?",(const char*[]){"secs"},&has_arg,&secs)) return NONE_VAL();
	time_t time = time_or_now(has_arg, secs);
	struct tm _time;
	if (!localtime_r(&time, &_time)) return krk_runtimeError(vm.exceptions->valueError, "?");

	return krk_asctime(&_time);
}

KRK_Function(gmtime) {
	int gave_seconds;
	long long seconds;
	if (!krk_parseArgs("|L?",(const char*[]){"secs"},&gave_seconds, &seconds)) return NONE_VAL();
	time_t time = time_or_now(gave_seconds, seconds);

	/* Create a struct_time to store result in */
	CURRENT_CTYPE out = (CURRENT_CTYPE)krk_newInstance(struct_time);
	krk_push(OBJECT_VAL(out));

	if (!gmtime_r(&time, &out->_value)) return krk_runtimeError(vm.exceptions->valueError, "?");

	return krk_pop();
}

KRK_Function(mktime) {
	struct struct_time_obj * t;
	if (!krk_parseArgs("O!",(const char*[]){"t"},struct_time,&t)) return NONE_VAL();

	struct tm _time;
	memcpy(&_time,&t->_value,sizeof(struct tm));
	_time.tm_wday = -1;
	time_t out = mktime(&_time);
	if (out == -1 && _time.tm_wday == -1) return krk_runtimeError(vm.exceptions->valueError, "invalid argument to mktime");
	return FLOATING_VAL(out);
}

KRK_Function(strftime) {
	const char * format;
	struct struct_time_obj * t = NULL;
	if (!krk_parseArgs("s|O!",(const char*[]){"format","t"},&format,struct_time,&t)) return NONE_VAL();
	struct tm _time;
	tm_or_now(t,&_time);

	/* strftime wants a buffer size, but we have no way of knowing. Following
	 * what CPython does, start from 1024 and try doubling until we reach
	 * the length of our format string * 256, and then give up. */
	size_t fmt_len = strlen(format);
	size_t size = 1024;
	while (1) {
		char * buf = malloc(size);
		size_t ret = strftime(buf,size,format,&_time);
		if (ret || size > fmt_len * 256) {
			krk_push(OBJECT_VAL(krk_copyString(buf,ret)));
			free(buf);
			return krk_pop();
		}
		size *= 2;
		free(buf);
	}
}

KRK_Module(time) {
	KRK_DOC(module, "@brief Provides timekeeping functions.");
	KRK_DOC(BIND_FUNC(module,sleep), "@brief Pause execution of the current thread.\n"
		"@arguments secs\n\n"
		"Uses the system @c usleep() function to sleep for @p secs seconds, which may be a @ref float or @ref int. "
		"The available precision is platform-dependent.");
	KRK_DOC(BIND_FUNC(module,time), "@brief Return the elapsed seconds since the system epoch.\n\n"
		"Returns a @ref float representation of the number of seconds since the platform's epoch date. "
		"On POSIX platforms, this is the number of seconds since 1 January 1970. "
		"The precision of the return value is platform-dependent.");

	krk_makeClass(module, &struct_time, "struct_time", KRK_BASE_CLASS(object));
	struct_time->allocSize = sizeof(struct struct_time_obj);
	KRK_DOC(struct_time, "Time value returned by various functions.");
	KRK_DOC(BIND_METHOD(struct_time,__init__), "@arguments iterable: tuple\n\n"
		"Create a @ref struct_time from a 9-tuple of @ref int values.\n"
		"The format of @p iterable is `(tm_year,tm_mon,tm_mday,tm_hour,tm_min,tm_sec,tm_wday,tm_yday,tm_isdst)`.");
	KRK_DOC(BIND_PROP(struct_time,tm_year),  "Calendar year");
	KRK_DOC(BIND_PROP(struct_time,tm_mon),   "Month, [1, 12]");
	KRK_DOC(BIND_PROP(struct_time,tm_mday),  "Day of the month, [1, 31]");
	KRK_DOC(BIND_PROP(struct_time,tm_hour),  "Clock hour, [0, 23]");
	KRK_DOC(BIND_PROP(struct_time,tm_min),   "Clock minute, [0, 59]");
	KRK_DOC(BIND_PROP(struct_time,tm_sec),   "Clock seconds, [0, 61] (maybe, due to leap seconds, depends on platform)");
	KRK_DOC(BIND_PROP(struct_time,tm_wday),  "Day of week, [0, 6], 0 is Monday.");
	KRK_DOC(BIND_PROP(struct_time,tm_yday),  "Day of year [1, 366]");
	KRK_DOC(BIND_PROP(struct_time,tm_isdst), "0, 1, -1 for unknown");
	BIND_METHOD(struct_time,__repr__);
	krk_finalizeClass(struct_time);

	KRK_DOC(BIND_FUNC(module,localtime), "@brief Convert seconds since epoch to local time.\n"
		"@arguments seconds=time.time()\n\n"
		"If @p seconds is not provided, the current @ref time is used.");
	KRK_DOC(BIND_FUNC(module,asctime), "@brief Convert time to string.\n"
		"@arguments t=time.localtime()\n\n"
		"If @p t is not provided, the current @ref localtime is used.");
	KRK_DOC(BIND_FUNC(module,ctime), "@brief Convert seconds since epoch to string.\n"
		"@arguments secs=time.time()\n\n"
		"If @p secs is not provided, the current @ref time is used.");
	KRK_DOC(BIND_FUNC(module,gmtime), "@brief Convert seconds since epoch to UTC time.\n"
		"@arguments secs=time.time()\n\n"
		"If @p secs is not provided, the current @ref time is used.");
	KRK_DOC(BIND_FUNC(module,mktime), "@brief Convert from local time to seconds since epoch.\n"
		"@arguments t\n\n"
		"For compatibility with @ref time a @ref float is returned.");
	KRK_DOC(BIND_FUNC(module,strftime), "@brief Format time string with system function.\n"
		"@arguments format,t=time.localtime()\n\n"
		"Uses the system `strftime` C function to convert a @ref struct_time to a string.\n"
		"If @p t is not provided, the current @ref localtime is used.");
}


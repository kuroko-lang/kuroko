/**
 * @brief Function call tracing tool
 * @file tools/callgrind.c
 * @author K. Lange <klange@toaruos.org>
 *
 * Generates cachegrind/callgrind trace files while running scripts.
 *
 * Traces include per-line instruction execution counts and per-function
 * (as well as per-call) wall clock timing information.
 *
 * Collecting instruction counts can be quite slow, so expect this to
 * increase runtime by as much as ~25×, depending on the code.
 */
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/debug.h>
#include <kuroko/util.h>

#include "common.h"

extern KrkValue krk_operator_add (KrkValue a, KrkValue b);
extern KrkValue krk_operator_sub (KrkValue a, KrkValue b);

static int usage(char * argv[]) {
	fprintf(stderr, "usage: %s [FILE] [args...]\n", argv[0]);
	return 1;
}

static KrkValue lineCache; /* {sourceCO: {line: count}} */
static KrkValue callCache; /* {sourceCO: {(codeobject,targetLine,sourceLine): [calls,instructions,time]}} */
static KrkValue timeCache; /* {sourceCO: time} */

static size_t lastFrameCount = 0;   /* Previously seen frame count, to track function entry/exit */
static size_t instrCounter = 0;     /* Total counter of executed instructions */
static size_t functionsEntered = 0; /* Number of function entries seen */

/**
 * @brief Calculate time difference as string.
 *
 * Calculates the number of nanoseconds since @p in_time and formats
 * the result as a string which can then be parsed into a @c long.
 *
 * @param in_time Start time to calculate from.
 * @param buf     String buffer to write into.
 */
static void time_diff(struct timespec in_time, char * buf) {
	struct timespec outTime, diff;
	clock_gettime(CLOCK_MONOTONIC, &outTime);
	diff.tv_sec  = outTime.tv_sec  - in_time.tv_sec;
	diff.tv_nsec = outTime.tv_nsec - in_time.tv_nsec;
	if (diff.tv_nsec < 0) {
		diff.tv_sec--;
		diff.tv_nsec += 1000000000L;
	}
	snprintf(buf,50,"%lld%.9ld", (long long)diff.tv_sec, diff.tv_nsec);
}

struct FrameMetadata {
	KrkCodeObject * target_obj; /* Function being entered */
	size_t target_line;         /* First line seen in the entered function */
	size_t count;               /* Instruction count on entry */
	size_t source_line;         /* Line we came from in the caller */
	KrkCodeObject * source_obj; /* Function we came from in the caller */
	struct timespec in_time;    /* Time of frame entry */
};

static struct FrameMetadata frameMetadata[KRK_CALL_FRAMES_MAX];

int krk_callgrind_debuggerHook(KrkCallFrame * frame) {
	instrCounter++;

	if (krk_currentThread.frameCount != lastFrameCount) {
		if (krk_currentThread.frameCount > lastFrameCount) {
			/* When we detect function entry, record details of the function being called
			 * and what called in, and record the current time. */
			KrkCallFrame * prev = lastFrameCount ? &krk_currentThread.frames[lastFrameCount-1] : NULL;
			frameMetadata[lastFrameCount].target_obj = frame->closure->function;
			frameMetadata[lastFrameCount].target_line = krk_lineNumber(&frame->closure->function->chunk, frame->ip - frame->closure->function->chunk.code);
			frameMetadata[lastFrameCount].count = instrCounter;
			frameMetadata[lastFrameCount].source_obj = prev ? prev->closure->function : NULL;
			frameMetadata[lastFrameCount].source_line = prev ? krk_lineNumber(&prev->closure->function->chunk, prev->ip - 1 - prev->closure->function->chunk.code) : 0;
			clock_gettime(CLOCK_MONOTONIC, &frameMetadata[lastFrameCount].in_time);
			functionsEntered++;
		} else {
			size_t procFrame = lastFrameCount - 1;
			while (procFrame >= krk_currentThread.frameCount) {
				if (procFrame == 0) {
					/* This is the outermost call, eg. <module>, returning to the interpreter. We still want to make
					 * sure we collect the time spent in the call and add it to the time for the function, so that
					 * the time spent inside other function calls is subtracted to get the time spent actually
					 * executing code in the module. */
					char tmp[50];
					time_diff(frameMetadata[procFrame].in_time, tmp);
					KrkValue diffTime = krk_parse_int(tmp,strlen(tmp),10);
					krk_push(diffTime);
					KrkValue totalTime = INTEGER_VAL(0);
					krk_tableGet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].target_obj),&totalTime);
					krk_push(krk_operator_add(totalTime,diffTime));
					krk_tableSet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].target_obj),krk_peek(0));
					krk_pop();
					krk_pop();
					break;
				}

				/* Set up dict for call mappings */
				KrkValue ndict = NONE_VAL();
				if (!krk_tableGet(AS_DICT(callCache),OBJECT_VAL(frameMetadata[procFrame].source_obj),&ndict)) {
					ndict = krk_dict_of(0,NULL,0);
					krk_push(ndict);
					krk_tableSet(AS_DICT(callCache),OBJECT_VAL(frameMetadata[procFrame].source_obj),ndict);
					krk_pop();
				}

				KrkTuple * t = krk_newTuple(3);
				krk_push(OBJECT_VAL(t));

				/* Target code object */
				t->values.values[t->values.count++] = OBJECT_VAL(frameMetadata[procFrame].target_obj);

				/* Target and source lines */
				t->values.values[t->values.count++] = INTEGER_VAL(frameMetadata[procFrame].target_line);
				t->values.values[t->values.count++] = INTEGER_VAL(frameMetadata[procFrame].source_line);

				KrkValue nlist = NONE_VAL();
				if (!krk_tableGet(AS_DICT(ndict),krk_peek(0),&nlist)) {
					nlist = krk_list_of(3,(const KrkValue[]){INTEGER_VAL(0),INTEGER_VAL(0),INTEGER_VAL(0)},0);
					krk_push(nlist);
					krk_tableSet(AS_DICT(ndict),krk_peek(1),krk_peek(0));
					krk_pop();
				}
				krk_pop(); /* tuple */

				/* totalCalls += 1 */
				krk_push(krk_operator_add(AS_LIST(nlist)->values[0],INTEGER_VAL(1)));
				AS_LIST(nlist)->values[0] = krk_pop();

				/* totalCosts += last cost */
				char tmp[50];
				snprintf(tmp,50,"%zu", instrCounter - frameMetadata[procFrame].count);
				KrkValue diffCount = krk_parse_int(tmp,strlen(tmp),10);
				krk_push(diffCount);
				krk_push(krk_operator_add(AS_LIST(nlist)->values[1],diffCount));
				AS_LIST(nlist)->values[1] = krk_pop();
				krk_pop(); /* diffCount */

				time_diff(frameMetadata[procFrame].in_time, tmp); /* Time for this call */
				KrkValue diffTime = krk_parse_int(tmp,strlen(tmp),10);
				krk_push(diffTime);
				krk_push(krk_operator_add(AS_LIST(nlist)->values[2],diffTime));
				AS_LIST(nlist)->values[2] = krk_pop();

				KrkValue myTime = INTEGER_VAL(0); /* Time spent here */
				krk_tableGet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].target_obj),&myTime);
				krk_push(krk_operator_add(myTime,diffTime));
				krk_tableSet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].target_obj),krk_peek(0));
				krk_pop(); /* myTime + diffTime */

				KrkValue parentTime = INTEGER_VAL(0); /* Was not spent there */
				krk_tableGet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].source_obj),&parentTime);
				krk_push(krk_operator_sub(parentTime,diffTime));
				krk_tableSet(AS_DICT(timeCache),OBJECT_VAL(frameMetadata[procFrame].source_obj),krk_peek(0));
				krk_pop(); /* parentTime - diffTime */

				krk_pop(); /* diffTime */

				memset(&frameMetadata[procFrame], 0, sizeof(struct FrameMetadata));
				procFrame--;
			}
		}
		lastFrameCount = krk_currentThread.frameCount;
	}

	if (!frame) return KRK_DEBUGGER_QUIT;

	/* Set up dict for instruction counts */
	KrkValue ndict = NONE_VAL();
	if (!krk_tableGet(AS_DICT(lineCache),OBJECT_VAL(frame->closure->function),&ndict)) {
		ndict = krk_dict_of(0,NULL,0);
		krk_push(ndict);
		krk_tableSet(AS_DICT(lineCache),OBJECT_VAL(frame->closure->function),ndict);
		krk_pop();
	}

	KrkValue lineNumber = INTEGER_VAL(krk_lineNumber(&frame->closure->function->chunk, frame->ip - frame->closure->function->chunk.code));

	/* Add one to instruction count for this line */
	KrkValue count = INTEGER_VAL(0);
	krk_tableGet(AS_DICT(ndict), lineNumber, &count);
	krk_push(krk_operator_add(count, INTEGER_VAL(1)));
	krk_tableSet(AS_DICT(ndict), lineNumber, krk_peek(0));
	krk_pop(); /* count + 1 */

	return KRK_DEBUGGER_STEP;
}


int main(int argc, char *argv[]) {
	char outfile[1024];
	snprintf(outfile,1024,"callgrind.out.%d",getpid());

	int opt;
	while ((opt = getopt(argc, argv, "+:f:-:")) != -1) {
		switch (opt) {
			case 'f':
				snprintf(outfile,1024,"%s", optarg);
				break;
			case '?':
				if (optopt != '-') {
					fprintf(stderr, "%s: unrocognized option '%c'\n", argv[0], optopt);
					return 1;
				}
				optarg = argv[optind]+1;
				/* fall through */
			case '-':
				if (!strcmp(optarg,"help")) {
				} else {
					fprintf(stderr, "%s: unrecognized option: '--%s'\n", argv[0], optarg);
					return 1;
				}
		}
	}
	if (optind == argc) {
		return usage(argv);
	}

	findInterpreter(argv);
	krk_initVM(KRK_THREAD_SINGLE_STEP);
	krk_debug_registerCallback(krk_callgrind_debuggerHook);
	addArgs(argc,argv);

	krk_startModule("__main__");

	lineCache = krk_dict_of(0,NULL,0);
	krk_attachNamedValue(&krk_currentThread.module->fields,"__line_cache__",lineCache);
	callCache = krk_dict_of(0,NULL,0);
	krk_attachNamedValue(&krk_currentThread.module->fields,"__call_cache__",callCache);
	timeCache = krk_dict_of(0,NULL,0);
	krk_attachNamedValue(&krk_currentThread.module->fields,"__time_cache__",timeCache);

	krk_runfile(argv[optind],argv[optind]);

	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
		krk_currentThread.flags &= ~(KRK_THREAD_HAS_EXCEPTION);
		fprintf(stderr, "== Executed ended by exception ==\n");
	} else {
		fprintf(stderr, "== Execution completed ==\n");
	}
	krk_callgrind_debuggerHook(NULL);

	fprintf(stderr, "%10zu total instruction%s\n", instrCounter, (instrCounter != 1) ? "s" : "");
	fprintf(stderr, "%10zu function%s calls\n", functionsEntered, (functionsEntered != 1) ? "s" : "");

	FILE * f = fopen(outfile,"w");
	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], outfile, strerror(errno));
		return 1;
	}

	fprintf(f,"# callgrind format\n");
	fprintf(f,"creator: Kuroko\n");
	fprintf(f,"positions: line\n");
	fprintf(f,"events: instructions nanoseconds\n");
	fprintf(f,"cmd: %s %s\n", argv[0], argv[optind]);
	fprintf(f,"summary: %zu ", instrCounter);
	char tmp[50];
	time_diff(frameMetadata[0].in_time, tmp);
	fprintf(f,"%s\n", tmp);


	for (size_t j = 0; j < AS_DICT(lineCache)->used; ++j) {
		if (IS_KWARGS(AS_DICT(lineCache)->entries[j].key)) continue;

		KrkCodeObject * function = (void*)AS_OBJECT(AS_DICT(lineCache)->entries[j].key);
		KrkValue        ndict    = AS_DICT(lineCache)->entries[j].value;

		fprintf(f,"fl=%s\n", function->chunk.filename->chars);
		fprintf(f,"fn=%s@%p\n", function->qualname ? function->qualname->chars : function->name->chars, (void*)function);

		KrkValue timeValue = NONE_VAL();
		krk_tableGet(AS_DICT(timeCache), OBJECT_VAL(function), &timeValue);
		if (!IS_NONE(timeValue)) {
			fprintf(f,"%zu ", (size_t)krk_lineNumber(&function->chunk, 0));
			struct StringBuilder sb = {0};
			krk_pushStringBuilderFormat(&sb, "0 %R\n", timeValue);
			fprintf(f,"%.*s", (int)sb.length, sb.bytes);
			krk_discardStringBuilder(&sb);
		}

		for (size_t k = 0; k < AS_DICT(ndict)->used; ++k) {
			if (IS_KWARGS(AS_DICT(ndict)->entries[k].key)) continue;
			struct StringBuilder sb = {0};
			krk_pushStringBuilderFormat(&sb, "%R %R 0\n", AS_DICT(ndict)->entries[k].key, AS_DICT(ndict)->entries[k].value);
			fprintf(f,"%.*s", (int)sb.length, sb.bytes);
			krk_discardStringBuilder(&sb);
		}

		KrkValue cdict = NONE_VAL();
		krk_tableGet(AS_DICT(callCache), OBJECT_VAL(function), &cdict);
		if (!IS_NONE(cdict)) {
			for (size_t l = 0; l < AS_DICT(cdict)->used; ++l) {
				if (IS_KWARGS(AS_DICT(cdict)->entries[l].key)) continue;

				KrkCodeObject * target = (void*)AS_OBJECT(AS_TUPLE(AS_DICT(cdict)->entries[l].key)->values.values[0]);
				KrkValue targetLine    = AS_TUPLE(AS_DICT(cdict)->entries[l].key)->values.values[1];
				KrkValue sourceLine    = AS_TUPLE(AS_DICT(cdict)->entries[l].key)->values.values[2];
				KrkValue totalCalls    = AS_LIST(AS_DICT(cdict)->entries[l].value)->values[0];
				KrkValue totalCost     = AS_LIST(AS_DICT(cdict)->entries[l].value)->values[1];
				KrkValue totalTime     = AS_LIST(AS_DICT(cdict)->entries[l].value)->values[2];

				fprintf(f,"cfi=%s\n", target->chunk.filename->chars);
				fprintf(f,"cfn=%s@%p\n", target->qualname ? target->qualname->chars : target->name->chars, (void*)target);

				struct StringBuilder sb = {0};
				krk_pushStringBuilderFormat(&sb, "calls=%R %R\n%R %R %R\n", totalCalls, targetLine, sourceLine, totalCost, totalTime);
				fprintf(f,"%.*s",(int)sb.length,sb.bytes);
				krk_discardStringBuilder(&sb);
			}
		}
	}

	fclose(f);

	krk_freeVM();
	return 0;
}


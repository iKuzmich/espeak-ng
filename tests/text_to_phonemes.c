/*
 * Debug tool for espeak_TextToPhonemesWithTerminator.
 *
 * Usage: test_text_to_phonemes [lang [text...]]
 * Defaults: lang=en, text=built-in samples (clause lookahead, merged words, expanded numbers)
 *
 * Prints per-clause phonemes, word-aligned phonemes, and the terminator
 * bitmask decoded into human-readable fields.
 *
 * The clause loop and sentence assembly mirror libpiper piper_synthesize_start, so the
 * per-sentence strings are the ones PiperNativeWrapper/AlignmentCalculator receive.
 * Each clause must yield one aligned group per source word; exits non-zero otherwise.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <espeak-ng/espeak_ng.h>
#include <espeak-ng/speak_lib.h>

#include "translate.h"
#include "readclause.h"

typedef struct {
	char *data;
	size_t len;
	size_t size;
} strbuf;

static void strbuf_append(strbuf *sb, const char *s)
{
	size_t n = strlen(s);
	if (sb->len + n + 1 > sb->size) {
		size_t size = (sb->len + n + 1) * 2;
		char *data = (char *)realloc(sb->data, size);
		if (data == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(EXIT_FAILURE);
		}
		sb->data = data;
		sb->size = size;
	}
	memcpy(sb->data + sb->len, s, n + 1);
	sb->len += n;
}

static void strbuf_clear(strbuf *sb)
{
	sb->len = 0;
	if (sb->data != NULL) {
		sb->data[0] = '\0';
	}
}

// Same source word split as GetWordAlignedPhonemeString Step 1.
static int count_source_words(const char *start, const char *end)
{
	int n = 0;
	int in_word = 0;
	for (const char *p = start; *p != '\0' && (end == NULL || p < end); p++) {
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			in_word = 0;
		} else if (!in_word) {
			in_word = 1;
			n++;
		}
	}
	return n;
}

// Groups are separated (not terminated) by single spaces; an empty slot shows up as consecutive spaces.
static int count_aligned_groups(const char *s)
{
	if (s == NULL || *s == '\0') {
		return 0;
	}
	int n = 1;
	for (; *s != '\0'; s++) {
		if (*s == ' ') {
			n++;
		}
	}
	return n;
}

// Same clause boundary as espeak_TextToPhonemesWithTerminator: lookahead chars ReadClause replays
// belong to the next clause. Must be called right after the clause was translated.
static const char *clause_end_of(const char *next, const char *start)
{
	int count = ReadClausePendingChars();
	const char *p = next;
	while (count-- > 0 && p > start) {
		p--;
		while (p > start && ((unsigned char)*p & 0xC0) == 0x80) {
			p--;
		}
	}
	return p;
}

// Same mapping as libpiper piper_synthesize_start.
static const char *terminator_str(int term)
{
	term &= 0x000FFFFF;
	if (term == CLAUSE_PERIOD) {
		return ".";
	} else if (term == CLAUSE_QUESTION) {
		return "?";
	} else if (term == CLAUSE_EXCLAMATION) {
		return "!";
	} else if (term == CLAUSE_COMMA) {
		return ", ";
	} else if (term == CLAUSE_COLON) {
		return ": ";
	} else if (term == CLAUSE_SEMICOLON) {
		return "; ";
	}
	return "";
}

// libpiper skips empty sentences and passes phonemes_aligned only when it differs from the regular phonemes.
static void flush_sentence(int *sentence, strbuf *phonemes, strbuf *aligned)
{
	if (phonemes->len > 0) {
		printf("  [sentence %d]\n", ++*sentence);
		printf("    phonemes         = \"%s\"\n", phonemes->data);
		if (aligned->len > 0 && strcmp(aligned->data, phonemes->data) != 0) {
			printf("    phonemes_aligned = \"%s\"\n", aligned->data);
		} else {
			printf("    phonemes_aligned = (not passed, regular phonemes used)\n");
		}
		printf("\n");
	}
	strbuf_clear(phonemes);
	strbuf_clear(aligned);
}

static const char *intonation_name(int term)
{
	switch (term & CLAUSE_INTONATION_TYPE) {
	case CLAUSE_INTONATION_FULL_STOP:   return "FULL_STOP";
	case CLAUSE_INTONATION_COMMA:       return "COMMA";
	case CLAUSE_INTONATION_QUESTION:    return "QUESTION";
	case CLAUSE_INTONATION_EXCLAMATION: return "EXCLAMATION";
	case CLAUSE_INTONATION_NONE:        return "NONE";
	default:                            return "?";
	}
}

static const char *type_name(int term)
{
	if (term & CLAUSE_TYPE_EOF)          return "EOF";
	if (term & CLAUSE_TYPE_VOICE_CHANGE) return "VOICE_CHANGE";
	if (term & CLAUSE_TYPE_SENTENCE)     return "SENTENCE";
	if (term & CLAUSE_TYPE_CLAUSE)       return "CLAUSE";
	return "NONE";
}

static void print_terminator(int term)
{
	int pause_raw = term & CLAUSE_PAUSE;
	int pause_ms  = (term & CLAUSE_PAUSE_LONG) ? pause_raw * 320 : pause_raw * 10;

	printf("    terminator   = 0x%08X\n", term);
	printf("      type       = %s\n",  type_name(term));
	printf("      intonation = %s\n",  intonation_name(term));
	printf("      pause      = %d ms%s\n", pause_ms,
	       (term & CLAUSE_PAUSE_LONG) ? " (LONG)" : "");
	if (term & CLAUSE_OPTIONAL_SPACE_AFTER)   printf("      flag: OPTIONAL_SPACE_AFTER\n");
	if (term & CLAUSE_PUNCTUATION_IN_WORD)    printf("      flag: PUNCTUATION_IN_WORD\n");
	if (term & CLAUSE_SPEAK_PUNCTUATION_NAME) printf("      flag: SPEAK_PUNCTUATION_NAME\n");
	if (term & CLAUSE_DOT_AFTER_LAST_WORD)    printf("      flag: DOT_AFTER_LAST_WORD\n");
}

// Returns the number of failed checks.
static int run_phoneme_mode(const char *lang, const char *text, int phonememode, const char *label)
{
	printf("--- %s ---\n", label);

	if (espeak_ng_SetPhonemizerVoiceByName(lang) != ENS_OK) {
		fprintf(stderr, "  espeak_ng_SetPhonemizerVoiceByName(\"%s\") failed\n", lang);
		return 1;
	}

	const void *textptr = text;
	const char *clause_start = text;
	strbuf sentence_phonemes = { NULL, 0, 0 };
	strbuf sentence_phonemes_aligned = { NULL, 0, 0 };
	int clause = 0;
	int sentence = 0;
	int total_words = 0;
	int total_groups = 0;
	int failures = 0;

	// Same loop condition as libpiper: no CLAUSE_TYPE_EOF break, runs until the decoder is drained
	while (textptr != NULL) {
		int terminator = 0;
		const char *word_phonemes = NULL;

		const char *phonemes = espeak_TextToPhonemesWithTerminator(&textptr, espeakCHARS_AUTO, phonememode, &terminator, &word_phonemes);
		const char *clause_end = (textptr != NULL) ? clause_end_of((const char *)textptr, clause_start) : NULL;

		// Decode failure leaves textptr untouched: libpiper would spin here forever
		if (phonemes == NULL) {
			fprintf(stderr, "  [clause %d] espeak_TextToPhonemesWithTerminator returned NULL\n", clause + 1);
			failures++;
			break;
		}

		int source_len = (clause_end != NULL) ? (int)(clause_end - clause_start) : (int)strlen(clause_start);
		int words = count_source_words(clause_start, clause_end);
		int groups = count_aligned_groups(word_phonemes);
		total_words += words;
		total_groups += groups;

		printf("  [clause %d]\n", ++clause);
		printf("    source       = \"%.*s\"\n", source_len, clause_start);
		printf("    phonemes     = \"%s\"\n", phonemes     ? phonemes     : "(null)");
		printf("    word_aligned = \"%s\"\n", word_phonemes ? word_phonemes : "(null)");
		printf("    words        = %d source, %d aligned%s\n", words, groups, (words == groups) ? "" : "  <-- MISMATCH");
		print_terminator(terminator);
		printf("    remaining    = \"%s\"\n", textptr ? (const char *)textptr : "(end of input)");
		printf("\n");

		if (words != groups) {
			failures++;
		}

		const char *term_str = terminator_str(terminator);
		strbuf_append(&sentence_phonemes, phonemes);
		if (word_phonemes != NULL) {
			strbuf_append(&sentence_phonemes_aligned, word_phonemes);
		}
		strbuf_append(&sentence_phonemes, term_str);
		strbuf_append(&sentence_phonemes_aligned, term_str);

		if ((terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE) {
			flush_sentence(&sentence, &sentence_phonemes, &sentence_phonemes_aligned);
		}

		if (clause_end != NULL) {
			clause_start = clause_end;
		}
	}
	flush_sentence(&sentence, &sentence_phonemes, &sentence_phonemes_aligned);

	// Clause spans must tile the whole text, otherwise lookahead chars were dropped or counted twice
	int text_words = count_source_words(text, NULL);
	if (text_words != total_words) {
		failures++;
	}

	printf("  summary: %d clause(s), %d sentence(s), words %d text / %d clauses / %d aligned, %d failure(s)\n\n",
	       clause, sentence, text_words, total_words, total_groups, failures);

	free(sentence_phonemes.data);
	free(sentence_phonemes_aligned.data);
	return failures;
}

int main(int argc, char **argv)
{
	const char *lang = (argc > 1) ? argv[1] : "en";
	static const char *const default_texts[] = {
		// clause/sentence terminators: ReadClause lookahead char carried into the next clause
		"Hello, world. How are you? I am fine! Testing: one, two; three.",
		// en_list multi-word entries: several source words merged into one phoneme word
		"We practise tai chi daily, graduated cum laude and went en masse.",
		// numbers and abbreviations: one source word expanded to several phoneme words
		"Mr. Smith bought 123 apples for 3.50 dollars in 2026.",
	};
	const char *const *texts = default_texts;
	int n_texts = (int)(sizeof(default_texts) / sizeof(default_texts[0]));
	if (argc > 2) {
		texts = (const char *const *)&argv[2];
		n_texts = argc - 2;
	}

	printf("espeak_TextToPhonemesWithTerminator debug\n");
	printf("  lang : \"%s\"\n\n", lang);

	espeak_ng_ERROR_CONTEXT ctx = NULL;
	espeak_ng_STATUS status = espeak_ng_InitializePhonemizer(NULL, NULL, &ctx);
	if (status != ENS_OK) {
		espeak_ng_PrintStatusCodeMessage(status, stderr, ctx);
		espeak_ng_ClearErrorContext(&ctx);
		return EXIT_FAILURE;
	}

	int failures = 0;
	for (int i = 0; i < n_texts; i++) {
		const char *text = texts[i];
		printf("  text : \"%s\"\n\n", text);

		// run_phoneme_mode(lang, text, 0, "ASCII phonemes");
		// printf("\n");
		failures += run_phoneme_mode(lang, text, espeakPHONEMES_IPA, "IPA phonemes");
	}

	printf("%s: %d failure(s)\n", (failures == 0) ? "PASS" : "FAIL", failures);

	espeak_Terminate();
	return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

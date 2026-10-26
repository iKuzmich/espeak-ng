/*
 * Debug tool for espeak_TextToPhonemesWithTerminator.
 *
 * Usage: test_text_to_phonemes [lang [text...]]
 * Defaults: lang=en, text=built-in samples (clause lookahead, merged words, expanded numbers)
 *
 * Prints per-clause phonemes, the text atom each phoneme word group comes from, and the terminator
 * bitmask decoded into human-readable fields.
 *
 * The clause loop and sentence assembly mirror libpiper piper_synthesize_start: a group is owned by
 * the ' '-separated atom of the whole text its source pointer falls into (Skidbladnir's CharPos atoms).
 * Exits non-zero when groups don't match the phoneme spaces, a source leaves its clause or points
 * at whitespace (sourceix base off), or atoms go backwards.
 */

#include "config.h"

#include <ctype.h>
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

static int count_spaces(const char *s)
{
	int n = 0;
	for (; *s != '\0'; s++) {
		if (*s == ' ') {
			n++;
		}
	}
	return n;
}

// Index of the ' '-separated atom of text containing p, as libpiper maps a group source.
static int atom_index(const char *text, const char *p)
{
	int n = 0;
	for (const char *q = text; q < p && *q != '\0'; q++) {
		if (*q == ' ') {
			n++;
		}
	}
	return n;
}

static const char *atom_start(const char *text, const char *p)
{
	while (p > text && p[-1] != ' ') {
		p--;
	}
	return p;
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

// libpiper skips empty sentences.
static void flush_sentence(int *sentence, strbuf *phonemes)
{
	if (phonemes->len > 0) {
		printf("  [sentence %d]\n", ++*sentence);
		printf("    phonemes = \"%s\"\n", phonemes->data);
		printf("\n");
	}
	strbuf_clear(phonemes);
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
	int clause = 0;
	int sentence = 0;
	int total_groups = 0;
	int last_atom = 0;
	int failures = 0;

	// Same loop condition as libpiper: no CLAUSE_TYPE_EOF break, runs until the decoder is drained
	while (textptr != NULL) {
		int terminator = 0;
		espeak_PHONEME_WORDS words_out = { 0, NULL };

		const char *phonemes = espeak_TextToPhonemesWithTerminator(&textptr, espeakCHARS_AUTO, phonememode, &terminator, &words_out);
		const char *clause_end = (textptr != NULL) ? clause_end_of((const char *)textptr, clause_start) : NULL;

		// Decode failure leaves textptr untouched: libpiper would spin here forever
		if (phonemes == NULL) {
			fprintf(stderr, "  [clause %d] espeak_TextToPhonemesWithTerminator returned NULL\n", clause + 1);
			failures++;
			break;
		}

		int source_len = (clause_end != NULL) ? (int)(clause_end - clause_start) : (int)strlen(clause_start);
		int spaces = count_spaces(phonemes);
		int groups_ok = (words_out.count == 0) ? (spaces == 0) : (words_out.count == spaces + 1);
		total_groups += words_out.count;

		printf("  [clause %d]\n", ++clause);
		printf("    source       = \"%.*s\"\n", source_len, clause_start);
		printf("    phonemes     = \"%s\"\n", phonemes);
		printf("    groups       = %d for %d space(s)%s\n", words_out.count, spaces, groups_ok ? "" : "  <-- MISMATCH");
		if (!groups_ok) {
			failures++;
		}

		// group i is the text between the i-th and (i+1)-th space of the phonemes
		const char *group = phonemes;
		for (int i = 0; i < words_out.count; i++) {
			const char *group_end = strchr(group, ' ');
			int group_len = (group_end != NULL) ? (int)(group_end - group) : (int)strlen(group);
			const char *source = words_out.sources[i];
			const char *slice_end = clause_start + source_len;
			int in_slice = (source >= clause_start && (source < slice_end || (source == clause_start && source_len == 0)));
			// word sources come from the sourceix of a word's first char, so a space means the base is off
			int on_word = (*source != '\0' && !isspace((unsigned char)*source));
			int atom = atom_index(text, source);
			const char *atom_text = atom_start(text, source);
			const char *error = !in_slice ? "  <-- OUTSIDE CLAUSE" : !on_word ? "  <-- SOURCE ON SPACE" : (atom < last_atom) ? "  <-- ATOM BACKWARDS" : NULL;

			printf("      [%d] \"%.*s\" -> atom %d \"%.*s\"%s\n", i, group_len, group, atom,
			       (int)strcspn(atom_text, " "), atom_text, error ? error : "");
			if (error) {
				failures++;
			}
			last_atom = atom;
			group = (group_end != NULL) ? group_end + 1 : group + group_len;
		}

		print_terminator(terminator);
		printf("    remaining    = \"%s\"\n", textptr ? (const char *)textptr : "(end of input)");
		printf("\n");

		strbuf_append(&sentence_phonemes, phonemes);
		strbuf_append(&sentence_phonemes, terminator_str(terminator));

		if ((terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE) {
			flush_sentence(&sentence, &sentence_phonemes);
		}

		if (clause_end != NULL) {
			clause_start = clause_end;
		}
	}
	flush_sentence(&sentence, &sentence_phonemes);

	printf("  summary: %d clause(s), %d sentence(s), %d group(s), %d atom(s), %d failure(s)\n\n",
	       clause, sentence, total_groups, atom_index(text, text + strlen(text)) + 1, failures);

	free(sentence_phonemes.data);
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
		// an ellipsis without a space ends a clause inside an atom; double spaces give empty atoms
		"Wait...what happened to  the dog?",
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

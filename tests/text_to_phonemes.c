/*
 * Debug tool for espeak_TextToPhonemesWithTerminator.
 *
 * Usage: test_text_to_phonemes [lang [text]]
 * Defaults: lang=en, text="Hello, world. How are you? I am fine!"
 *
 * Prints per-clause phonemes, word-aligned phonemes, and the terminator
 * bitmask decoded into human-readable fields.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <espeak-ng/espeak_ng.h>
#include <espeak-ng/speak_lib.h>

#include "translate.h"

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

static void run_phoneme_mode(const char *lang, const char *text, int phonememode, const char *label)
{
	printf("--- %s ---\n", label);

	if (espeak_SetVoiceByName(lang) != EE_OK) {
		fprintf(stderr, "  espeak_SetVoiceByName(\"%s\") failed\n", lang);
		return;
	}

	const void *textptr = text;
	int clause = 0;

	while (1) {
		int terminator = 0;
		const char *word_phonemes = NULL;

		const char *phonemes = espeak_TextToPhonemesWithTerminator(&textptr, espeakCHARS_AUTO, phonememode, &terminator, &word_phonemes);

		printf("  [clause %d]\n", ++clause);
		printf("    phonemes     = \"%s\"\n", phonemes     ? phonemes     : "(null)");
		printf("    word_aligned = \"%s\"\n", word_phonemes ? word_phonemes : "(null)");
		print_terminator(terminator);
		printf("    remaining    = \"%s\"\n", textptr ? (const char *)textptr : "(end of input)");
		printf("\n");

		if (terminator & CLAUSE_TYPE_EOF || textptr == NULL)
			break;
	}
}

int main(int argc, char **argv)
{
	const char *lang = (argc > 1) ? argv[1] : "en";
	const char *text = (argc > 2) ? argv[2]
	    : "Hello, world. How are you? I am fine! Testing: one, two; three.";

	printf("espeak_TextToPhonemesWithTerminator debug\n");
	printf("  lang : \"%s\"\n", lang);
	printf("  text : \"%s\"\n\n", text);

	espeak_ng_InitializePath(NULL);
	espeak_ng_ERROR_CONTEXT ctx = NULL;
	if (espeak_ng_Initialize(&ctx) != ENS_OK) {
		fprintf(stderr, "espeak_ng_Initialize failed\n");
		return EXIT_FAILURE;
	}
	if (espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, 0, NULL) != ENS_OK) {
		fprintf(stderr, "espeak_ng_InitializeOutput failed\n");
		return EXIT_FAILURE;
	}

	// run_phoneme_mode(lang, text, 0, "ASCII phonemes");
	// printf("\n");
	run_phoneme_mode(lang, text, espeakPHONEMES_IPA, "IPA phonemes");

	espeak_Terminate();
	return EXIT_SUCCESS;
}

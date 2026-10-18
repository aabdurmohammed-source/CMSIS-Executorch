#ifndef VOICE_H_
#define VOICE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Few-shot keyword spotting: MFCC features + DTW against templates you teach
 * by saying each word a few times. */

#define VOICE_WORDS       2     /* 0 = jump, 1 = dance */
#define VOICE_TEMPLATES   6     /* recordings kept per word (newest replace oldest): two people x 3 */
#define VOICE_MAX_FRAMES  120   /* 1.2 s of speech */

/* Finds the speech in 8 kHz mono samples and computes its features.
 * Returns the number of 10 ms frames of speech, 0 if nothing was said. */
int voice_features(const int16_t *pcm, int n);

/* Stores the last features as a template of word. Returns its template count. */
int voice_add_template(int word);

/* Templates taught for word so far */
int voice_count(int word);

/* Compares the last features with the templates: the word, -1 if nothing is
 * taught (or nothing was heard), -2 if it sounded like none of the words. */
int voice_match(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_H_ */

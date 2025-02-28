#ifndef UTIL_H_
#define UTIL_H_

#include "config.h"

#include <string.h>
#if HAVE_STRINGS_H
#include <strings.h>
#endif
#include <math.h>
#include <ctype.h>
#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* String functions ------------------------------------------------------ */

/* Returns TRUE if the characters are equal or represent the same letter
   in different case. */
int Util_chrieq(char c1, char c2);

inline static int stricmp(const char* s1, const char* s2) {
   size_t l1 = strlen(s1);
   size_t l2 = strlen(s2);
   if (l1 > l2) return 1;
   if (l1 < l2) return -1;
   for (size_t i = 0; i < l1; ++i) {
      char c1 = tolower(s1[i]);
      char c2 = tolower(s2[i]);
      if (c1 > c2) return 1;
      if (c2 < c2) return -1;
   }
   return 0;
}

#ifdef __STRICT_ANSI__
/* Returns a positive integer if str1>str2, negative if str1<str2
 * 0 if str1 == str2, case insensitive */
int Util_stricmp(const char *str1, const char *str2);
#elif defined(HAVE_WINDOWS_H)
#define Util_stricmp _stricmp
#elif defined(HAVE_STRCASECMP)
#define Util_stricmp strcasecmp
#else
#define Util_stricmp stricmp
#endif

/* Returns TRUE if str1 ends with the characters in str2, regardless of case. */
int Util_striendswith(const char *str1, const char *str2);

/* Same as strncasecmp(), compare 2 strings, limited to size. */
int Util_strnicmp(const char *str1, const char *str2, size_t size);

/* Same as stpcpy() in some C libraries: copies src to dest
   and returns a pointer to the trailing NUL in dest. */
char *Util_stpcpy(char *dest, const char *src);

/* NestedVM strncpy from newlib has a bug */
#ifdef HAVE_STRNCPY
#define Util_strncpy strncpy
#else
char *Util_strncpy(char *dest, const char *src, size_t size);
#endif

/* Same as strlcpy() in some C libraries: copies src to dest
   and terminates the string. Never writes more than size characters
   to dest (the result may be truncated). Returns dest. */
char *Util_strlcpy(char *dest, const char *src, size_t size);

/* Modifies the string to uppercase and returns it. */
char *Util_strupper(char *s);

/* Modifies the string to lowercase and returns it. */
char *Util_strlower(char *s);

/* Similar to Perl's chomp(): removes trailing LF, CR or CR/LF. */
void Util_chomp(char *s);

/* Similar to Basic's trim(): removes leading and trailing whitespace. */
void Util_trim(char *s);

/* Converts the string to a non-negative integer and returns it.
   The string must represent the number and nothing else.
   -1 indicates an error. */
int Util_sscandec(const char *s);

/* Likewise, but parses hexadecimal numbers. */
int Util_sscanhex(const char *s);

/* Likewise, but allows only 0 and 1. */
int Util_sscanbool(const char *s);

/* Converts the string S to a signed integer *DEST and returns a success flag.
   The string must be a decimal number optionally preceded by a + or - sign. */
int Util_sscansdec(char const *s, int *dest);

/* Converts the string S to a (signed) floating point number *DEST and returns
   a success flag. The string must be a floating-point number. */
int Util_sscandouble(char const *s, double *dest);

/* safe_strncpy guarantees that the dest. buffer ends with '\0' */
char *safe_strncpy(char *, const char *, int);

/* Math functions -------------------------------------------------------- */

/* Rounds the floating-point number to the nearest integer. */
#if HAVE_ROUND
#define Util_round round
#else
double Util_round(double x);
#endif

/* Function for comparing double floating-point numbers. */
#define Util_almostequal(x, y, epsilon) (fabs((x)-(y)) <= (epsilon))

/* Memory management ----------------------------------------------------- */

/* malloc() with out-of-memory checking. Never returns NULL. */
void *Util_malloc(size_t size, const char* from);

/* realloc() with out-of-memory checking. Never returns NULL. */
void *Util_realloc(void *ptr, size_t size, const char* from);

/* strdup() with out-of-memory checking. Never returns NULL. */
char *Util_strdup(const char *s, const char* from);


/* Filenames ------------------------------------------------------------- */

/* I assume here that '\n' is not valid in filenames,
   at least not as their first character. */
#define Util_FILENAME_NOT_SET               "\n"
#define Util_filenamenotset(filename)  ((filename)[0] == '\n')

#ifdef DIR_SEP_BACKSLASH
#define Util_DIR_SEP_CHAR '\\'
#define Util_DIR_SEP_STR  "\\"
#else
#define Util_DIR_SEP_CHAR '/'
#define Util_DIR_SEP_STR  "/"
#endif

/* Splits a filename into directory part and file part. */
/* dir_part or file_part may be NULL. */
void Util_splitpath(const char *path, char *dir_part, char *file_part);

/* Concatenates file paths.
   Places directory separator char between paths, unless path1 is empty
   or ends with the separator char, or path2 starts with the separator char. */
void Util_catpath(char *result, const char *path1, const char *path2);

/* Takes input string p in the form of "foo%bar##.ext" and converts to "foo%%bar%02d.ext",
   storing the result in the char array pointed to by buffer (with a max size bufsize).
   At most 9 digits are supported; if more that that, uses the format specifed in default.
   Default format should use the hash specification also, like "atari###.ext".

   Returns the maximum number supported by the pattern. */
int Util_filenamepattern(const char *p, char *buffer, int bufsize, const char *default_pattern);

/* Find the next available filename given by format (which can be found using a call to
   Util_filenamepattern). The maximum value must be given in no_max, and the returned
   filename is stored in buffer, with max bufsize characters. The no_last pointer is
   used to remember the last successful value. no_last should be initialized with a -1
   before calling this function for the first time.

   Returns TRUE if successful, or FALSE if allow_overwrite is FALSE and could not find
   an unused filename. Will always be successful if allow_overwrite is TRUE. */
int Util_findnextfilename(const char *format, int *no_last, int no_max, char *buffer, int bufsize, int allow_overwrite);


/* File I/O -------------------------------------------------------------- */

/* Returns TRUE if the specified file exists. */
int Util_fileexists(const char *filename);

/* Returns TRUE if the specified directory exists. */
int Util_direxists(const char *filename);

/* Rewinds the stream to its beginning. */
#ifdef HAVE_REWIND
#define Util_rewind(fp) rewind(fp)
#else
#define Util_rewind(fp) f_lseek(fp, 0)
#endif

/* Deletes a file, returns 0 on success, -1 on failure. */
#ifdef HAVE_WINDOWS_H
int Util_unlink(const char *filename);
#define HAVE_UTIL_UNLINK
#elif defined(HAVE_UNLINK)
#define Util_unlink  unlink
#define HAVE_UTIL_UNLINK
#endif /* defined(HAVE_UNLINK) */

#include "ff.h"

/* Creates a file that does not exist and fills in filename with its name. */
FIL *Util_uniqopen(char *filename, const char *mode);

/* Support for temporary files.

   Util_tmpbufdef() defines storage for names of temporary files, if necessary.
     Example use:
     Util_tmpbufdef(static, mytmpbuf[4]) // four buffers with "static" storage
   Util_fopen() opens a *non-temporary* file and marks tmpbuf
   as *not containing* name of a temporary file.
   Util_tmpopen() creates a temporary file with "wb+" mode.
   Util_fclose() closes a file. If it was temporary, it gets deleted.

   There are 3 implementations of the above:
   - one that uses tmpfile() (preferred)
   - one that stores names of temporary files and deletes them when they
     are closed
   - one that creates unique files but doesn't delete them
     because Util_unlink is not available
*/
#ifdef HAVE_TMPFILE
#define Util_tmpbufdef(modifier, def)
#define Util_fopen(filename, mode, tmpbuf)  fopen(filename, mode)
#define Util_tmpopen(tmpbuf)                tmpfile()
#define Util_fclose(fp, tmpbuf)             fclose(fp)
#elif defined(HAVE_UTIL_UNLINK)
#define Util_tmpbufdef(modifier, def)       modifier char def [FILENAME_MAX];
#define Util_fopen(filename, mode, tmpbuf)  (tmpbuf[0] = '\0', fopen(filename, mode))
#define Util_tmpopen(tmpbuf)                Util_uniqopen(tmpbuf, "wb+")
#define Util_fclose(fp, tmpbuf)             (fclose(fp), tmpbuf[0] != '\0' && Util_unlink(tmpbuf))
#else
/* if we can't delete the created file, leave it to the user */
#define Util_tmpbufdef(modifier, def)
///#define Util_fopen(filename, mode, tmpbuf)  fopen(filename, mode)
///#define Util_tmpopen(tmpbuf)                Util_uniqopen(NULL, "wb+")
#define Util_fclose(fp, tmpbuf)             f_close(fp)
#endif

void Util_sleep(double s);
double Util_time(void);

/* Get current working directory. */
char *Util_getcwd(char *buf, size_t size);

#include "ff.h"
/* Returns the length of an open stream.
   May change the current position. */
int Util_flen(FIL *fp);

#endif /* UTIL_H_ */

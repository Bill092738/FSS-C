#ifndef FSS_RULES_LOAD_H
#define FSS_RULES_LOAD_H

/* Applies overrides from a JSON file to FSS_RULES. A missing file is not an
 * error. Returns 0 on success, -1 on malformed input or unknown keys. */
int fss_rules_load(const char *path);

#endif /* FSS_RULES_LOAD_H */

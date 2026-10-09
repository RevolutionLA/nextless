#pragma once

#include <string>

namespace nextless {

// Serialize a user-supplied string as a JSON string literal (quoted, with the
// handful of escapes these config files actually need). Paths and API keys go
// through here, so backslashes and quotes must not break the document.
std::string jsonQuote(const std::string &value);

inline std::string jsonNum(int v) { return std::to_string(v); }
inline std::string jsonBool(bool v) { return v ? "true" : "false"; }

// Set `key: rawValue` inside the object reached by following `section` (empty
// = document top level). `rawValue` is already a valid JSON literal (use
// jsonQuote/jsonNum/jsonBool). Existing keys are replaced in place; unknown
// keys are inserted; everything else in the text is left untouched, so hand
// added keys and formatting survive. Returns true if the text changed.
//
// If `section` is named but absent, the whole section object is created before
// the top-level close. Malformed input (no object to write into) is a no-op.
bool patchJson(std::string &text, const std::string &section,
               const std::string &key, const std::string &rawValue);

// Write `contents` to `path` via a temp file + rename, creating parent dirs.
// Returns false on any failure (never leaves a half-written file).
bool writeFileAtomic(const std::string &path, const std::string &contents);

} // namespace nextless

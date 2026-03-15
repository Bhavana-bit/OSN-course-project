#ifndef ERRORS_H
#define ERRORS_H

// ==============================
// Unified String Error Constants
// ==============================

// 4xx Client Errors
#define E400_INVALID_CMD   "E400_INVALID_CMD"
#define E401_UNAUTHORIZED  "E401_UNAUTHORIZED"
#define E403_FORBIDDEN     "E403_FORBIDDEN"
#define E404_NOT_FOUND     "E404_NOT_FOUND"
#define E409_CONFLICT      "E409_CONFLICT"
#define E423_LOCKED        "E423_LOCKED"

// 5xx Server Errors
#define E500_INTERNAL      "E500_INTERNAL"
#define E503_NM_FAILURE    "E503_NM_FAILURE"

// Success
#define OK200_SUCCESS      "OK200_SUCCESS"

// ==========================================
// Function declarations for use in .c files
// ==========================================
const char *error_message(const char *code);
void send_error(int sock, const char *code);

#endif

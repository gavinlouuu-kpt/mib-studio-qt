# Use bounded byte arrays, not string literals. Adjacent literals are not a fix:
# MSVC concatenates them before applying its C2026 aggregate string limit.
function(mib_generate_review_isoelastic input output)
    # HEX avoids file(READ)'s text newline conversion on CRLF resources.
    file(READ "${input}" data HEX)
    string(LENGTH "${data}" hex_size)
    math(EXPR MIB_REVIEW_ISOELASTIC_SIZE "${hex_size} / 2")
    set(MIB_REVIEW_ISOELASTIC_DECLARATIONS "")
    set(MIB_REVIEW_ISOELASTIC_APPENDS "")
    set(offset 0)
    set(index 0)
    while(offset LESS hex_size)
        string(SUBSTRING "${data}" ${offset} 8192 chunk)
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," chunk "${chunk}")
        string(APPEND MIB_REVIEW_ISOELASTIC_DECLARATIONS
            "inline constexpr unsigned char kReviewIsoelasticChunk${index}[] = {${chunk}};\n")
        string(APPEND MIB_REVIEW_ISOELASTIC_APPENDS
            "    data.append(reinterpret_cast<const char*>(kReviewIsoelasticChunk${index}), sizeof(kReviewIsoelasticChunk${index}));\n")
        math(EXPR offset "${offset} + 8192")
        math(EXPR index "${index} + 1")
    endwhile()
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/ReviewIsoelasticData.h.in"
                   "${output}" @ONLY)
endfunction()

# Standalone entry point for the compiler/byte-roundtrip regression.
if(DEFINED INPUT AND DEFINED OUTPUT)
    mib_generate_review_isoelastic("${INPUT}" "${OUTPUT}")
endif()

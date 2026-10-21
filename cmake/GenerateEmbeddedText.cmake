# Compile a resource file's exact bytes into a header as `inline std::string
# <function>()` (#764: the bundled E-modulus LUT in mib_review_core, so a
# review shell without the resources folder can still recompute E with it
# when a run recorded that table's hash). Same approach as
# GenerateReviewIsoelastic.cmake: bounded byte arrays, not string literals
# (MSVC applies its C2026 aggregate string limit after concatenating adjacent
# literals), and HEX so CRLF resources keep their bytes.
function(mib_generate_embedded_text input output function_name)
    file(READ "${input}" data HEX)
    string(LENGTH "${data}" hex_size)
    math(EXPR MIB_EMBEDDED_TEXT_SIZE "${hex_size} / 2")
    set(MIB_EMBEDDED_TEXT_DECLARATIONS "")
    set(MIB_EMBEDDED_TEXT_APPENDS "")
    set(offset 0)
    set(index 0)
    while(offset LESS hex_size)
        string(SUBSTRING "${data}" ${offset} 8192 chunk)
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," chunk "${chunk}")
        string(APPEND MIB_EMBEDDED_TEXT_DECLARATIONS
            "inline constexpr unsigned char k${function_name}Chunk${index}[] = {${chunk}};\n")
        string(APPEND MIB_EMBEDDED_TEXT_APPENDS
            "    data.append(reinterpret_cast<const char*>(k${function_name}Chunk${index}), sizeof(k${function_name}Chunk${index}));\n")
        math(EXPR offset "${offset} + 8192")
        math(EXPR index "${index} + 1")
    endwhile()
    set(MIB_EMBEDDED_TEXT_FUNCTION "${function_name}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/EmbeddedText.h.in" "${output}" @ONLY)
endfunction()

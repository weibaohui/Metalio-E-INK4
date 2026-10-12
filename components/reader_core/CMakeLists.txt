idf_component_register(
    SRCS "src/zip_reader.cc"
         "src/html_content.cc"
         "src/text_encoding.cc"
         "src/txt_chapter.cc"
         "src/epub_document.cc"
         "src/ebook_document.cc"
         "src/image_stream.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/reader_core"
    REQUIRES espressif__zlib lvgl__lvgl
)

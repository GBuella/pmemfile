
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <clang-c/Index.h>

static const char *prologue =
	"/* Generated source file, do not edit manually! */\n"
	"\n"
	"#ifndef LIBPMEMFILE_POSIX_H_WRAPPERS\n"
	"#define LIBPMEMFILE_POSIX_H_WRAPPERS\n"
	"\n"
	"#include \"libpmemfile-posix.h\"\n"
	"#include \"preload.h\"\n"
	"#include <stdint.h>\n"
	"\n";

static const char *epilogue =
	"\n"
	"#endif\n";


#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static const char prefix[] = "wrapper_";

struct arg_desc {
	CXType type;
	CXString type_name_ref;
	const char *type_name;
	CXString name_ref;
	const char *name;
};

struct func_desc {
	CXString name_ref;
	bool is_void;
	const char *name;
	bool is_variadic;
	CXType return_type;
	CXString return_type_name_ref;
	const char *return_type_name;
	enum CX_StorageClass storage;
	int arg_count;
	struct arg_desc args[0x20];
};

static struct func_desc
collect_data(CXCursor func_decl)
{
	struct func_desc desc;

	desc.is_variadic = clang_Cursor_isVariadic(func_decl) != 0;
	desc.name_ref = clang_getCursorSpelling(func_decl);
	desc.name = clang_getCString(desc.name_ref);
	desc.storage = clang_Cursor_getStorageClass(func_decl);
	desc.arg_count = clang_Cursor_getNumArguments(func_decl);
	desc.return_type = clang_getCursorResultType(func_decl);
	desc.return_type_name_ref = clang_getTypeSpelling(desc.return_type);
	desc.return_type_name = clang_getCString(desc.return_type_name_ref);
	desc.is_void = (strcmp(desc.return_type_name, "void") == 0);

	for (int i = 0; i < desc.arg_count; ++i) {
		struct arg_desc *arg = desc.args + i;
		CXCursor arg_cursor = clang_Cursor_getArgument(func_decl, i);
		arg->name_ref = clang_getCursorSpelling(arg_cursor); /* XXX */
		arg->name = clang_getCString(arg->name_ref); /* XXX */

		arg->type = clang_getCursorType(arg_cursor);
		arg->type_name_ref = clang_getTypeSpelling(arg->type);
		arg->type_name = clang_getCString(arg->type_name_ref);

		if (arg->name == NULL || arg->name[0] == '\0') {
			if (strcmp(arg->type_name, "PMEMfilepool *") == 0) {
				arg->name = "pfp";
			} else if (strcmp(arg->type_name, "PMEMfile *") == 0) {
				arg->name = "file";
			} else {
				fprintf(stderr, "%s has unnamed argument",
					desc.name);
				exit(1);
			}
		}
	}

	return desc;
}

static void
dispose_desc(struct func_desc desc)
{
	clang_disposeString(desc.name_ref);
	clang_disposeString(desc.return_type_name_ref);
	for (int i = 0; i < desc.arg_count; ++i) {
		clang_disposeString(desc.args[i].type_name_ref);
		clang_disposeString(desc.args[i].name_ref);
	}
}

static void
print_type_and_name(const char *type, const char *name)
{
	if (type[strlen(type) - 1] == '*')
		printf("%s%s", type, name);
	else
		printf("%s %s", type, name);
}

static void
print_prototype(struct func_desc desc)
{
	printf("static inline %s\n", desc.return_type_name);
	printf("%s%s(", prefix, desc.name);

	if (desc.arg_count == 0)
		printf("void");

	for (int i = 0; i < desc.arg_count; ++i) {
		if (i > 0)
			printf(", ");
		print_type_and_name(desc.args[i].type_name, desc.args[i].name);
	}
	puts(")");
}

static void
print_forward_call(struct func_desc desc)
{
	printf("%s(", desc.name);
	for (int i = 0; i < desc.arg_count; ++i) {
		if (i > 0)
			printf(", ");
		printf("%s", desc.args[i].name);
	}
	puts(");");
}

static bool
is_printable_cstr_type(const char *type_name)
{
	static const char *const accepted_types[] = {
		"const char *"
	};

	for (size_t i = 0; i < ARRAY_SIZE(accepted_types); ++i)
		if (strcmp(type_name, accepted_types[i]) == 0)
			return true;

	return false;
}

static bool
is_printable_cstr_name(const char *name)
{
	static const char *const accepted_names[] = {
		"path",
		"pathname",
		"oldpath",
		"newpath",
		"old_path",
		"new_path"
	};

	for (size_t i = 0; i < ARRAY_SIZE(accepted_names); ++i)
		if (strcmp(name, accepted_names[i]) == 0)
			return true;

	return false;
}

static bool
is_arg_printable_cstr(const char *type_name, const char *name)
{
	return is_printable_cstr_type(type_name) &&
		is_printable_cstr_name(name);
}

static bool
is_signed_int(enum CXTypeKind kind)
{
	return kind == CXType_Int ||
		kind == CXType_Short ||
		kind == CXType_Long ||
		kind == CXType_LongLong;
}

static void
print_format(CXType type, const char *type_name, const char *name)
{
	if (is_arg_printable_cstr(type_name, name))
		fputs("\\\"%s\\\"", stdout);
	else if (strcmp(type_name, "size_t") == 0)
		fputs("%zu", stdout);
	else if (strcmp(type_name, "pmemfile_ssize_t") == 0)
		fputs("%z", stdout);
	else if (strcmp(type_name, "pmemfile_mode_t") == 0)
		fputs("%3jo", stdout);
	else if (type.kind == CXType_Pointer)
		fputs("%p", stdout);
	else if (is_signed_int(type.kind))
		fputs("%j", stdout);
	else /* treating it as an unsigned integral type */
		fputs("%jx", stdout);
}

static bool
is_pointer_to_const(CXType type)
{
	if (type.kind != CXType_Pointer)
		return false;

	CXType pointee = clang_getPointeeType(type);
	return clang_isConstQualifiedType(pointee) != 0;
}

static void
print_format_argument(CXType type, const char *type_name, const char *name)
{
	if (is_arg_printable_cstr(type_name, name))
		fputs(name, stdout);
	else if (strcmp(type_name, "size_t") == 0)
		fputs(name, stdout);
	else if (strcmp(type_name, "pmemfile_ssize_t") == 0)
		fputs(name, stdout);
	else if (is_pointer_to_const(type))
		printf("(const void *)%s", name);
	else if (type.kind == CXType_Pointer)
		printf("(void *)%s", name);
	else if (is_signed_int(type.kind))
		printf("(intmax_t)%s", name);
	else
		printf("(uintmax_t)%s", name);
}

static void
print_log_write(struct func_desc desc)
{

	printf("\tlog_write(\"%s(", desc.name);

	for (int i = 0; i < desc.arg_count; ++i) {
		if (i > 0)
			printf(", ");

		print_format(desc.args[i].type,
				desc.args[i].type_name,
				desc.args[i].name);
	}
	printf(")");

	if (!desc.is_void) {
		printf(" = ");
		print_format(desc.return_type, desc.return_type_name, "");
	}
	printf("\"");


	for (int i = 0; i < desc.arg_count; ++i) {
		printf(", ");
		print_format_argument(desc.args[i].type,
				desc.args[i].type_name,
				desc.args[i].name);
	}

	if (!desc.is_void) {
		printf(", ");
		print_format_argument(desc.return_type,
				desc.return_type_name, "");
		printf("ret");
	}

	puts(");");
}

static void
print_errno_handler(void)
{
	puts("\tif (ret < 0)");
	puts("\t\tret = -errno;");
}

static void
print_wrapper(struct func_desc desc)
{
	print_prototype(desc);
	puts("{");

	if (desc.is_void) {
		printf("\t");
	} else {
		putchar('\t');
		print_type_and_name(desc.return_type_name, "ret");
		printf(";\n\n\tret = ");
	}

	print_forward_call(desc);


	if (strcmp(desc.return_type_name, "int") == 0 ||
	    strcmp(desc.return_type_name, "pmemfile_ssize_t") == 0)
		print_errno_handler();

	putchar('\n');
	print_log_write(desc);
	putchar('\n');

	if (!desc.is_void) {
		puts("");
		puts("\treturn ret;");
	}

	puts("}");
	puts("");
}

static bool
is_relevant_func_decl(CXCursor cursor)
{
	if (clang_getCursorKind(cursor) != CXCursor_FunctionDecl)
		return false;

	if (clang_Cursor_isVariadic(cursor) != 0)
		return false;

	static const char orig_prefix[] = "pmemfile_";

	CXString name_ref = clang_getCursorSpelling(cursor);
	const char *name = clang_getCString(name_ref);
	bool ret = strncmp(name, orig_prefix, strlen(orig_prefix)) == 0;
	clang_disposeString(name_ref);

	return ret;
}

static enum CXChildVisitResult
visitor(CXCursor cursor, CXCursor parent, CXClientData client_data)
{
	if (is_relevant_func_decl(cursor)) {
		struct func_desc desc = collect_data(cursor);

		print_wrapper(desc);
		dispose_desc(desc);
		return CXChildVisit_Continue;
	}

	return CXChildVisit_Recurse;
}

int main(int argc, const char **argv)
{
	if (argc < 2)
		return 1;

	CXIndex index = clang_createIndex(0, 0);
	CXTranslationUnit unit = clang_parseTranslationUnit(
			index,
			argv[1], NULL, 0,
			NULL, 0,
			CXTranslationUnit_None);

	if (unit == NULL)
		return 1;

	CXCursor cursor = clang_getTranslationUnitCursor(unit);

	fputs(prologue, stdout);

	clang_visitChildren(cursor, visitor, NULL);

	fputs(epilogue, stdout);

	clang_disposeTranslationUnit(unit);
	clang_disposeIndex(index);


	return 0;
}

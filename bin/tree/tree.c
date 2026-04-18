#include <stdio.h>
#include <string.h>
#include <dirent.h>  
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static void print_tree(const char *path, int depth)
{
	DIR *dir = opendir(path);

    if (dir == NULL) 
    {
        perror(path);
        return;
    }

	struct stat entry_stats;
	char fullpath[PATH_MAX];

    struct dirent *entry;

	while ((entry = readdir(dir)) != NULL) 
    {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
            continue;

		for (int i = 0; i < depth; i++)
			printf("--");

		printf("%s\n", entry->d_name);

		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        int check = lstat(fullpath, &entry_stats);

		if (!(check == -1) && S_ISDIR(entry_stats.st_mode) && !S_ISLNK(entry_stats.st_mode))
		    print_tree(fullpath, depth + 1);
	}

	closedir(dir);
}

int main(int argc, char *argv[])
{
	const char *path;
	char cwd[PATH_MAX];

	if (argc > 1)
		path = argv[1];
	else 
    {
		getcwd(cwd, sizeof(cwd));
		path = cwd;
	}

	printf("%s\n", path);
	print_tree(path, 1);

	return 0;
}
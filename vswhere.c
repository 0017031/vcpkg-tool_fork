// cl.exe vswhere.c  /link  /NODEFAULTLIB  /ENTRY:main  Kernel32.lib
// #include <windows.h>
#include <stdio.h>
int main() {
    const char* xmlBody =
	"<?xml version=\"1.0\"?>\n"
        "<instances>\n"
        "  <instance>\n"
        "    <instanceId>VisualStudio.17.10</instanceId>\n"
        "    <installationPath>D:\\m3\\vs2022_17.10\\</installationPath>\n"
        "    <installationVersion>17.10</installationVersion>\n"
		"    <isPrerelease>0</isPrerelease>\n"
        "  </instance>\n"
        "</instances>\n";

    // HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    // DWORD written;

    // WriteConsoleA(hStdOut, xmlBody, lstrlenA(xmlBody), &written, NULL);

	printf("%s", xmlBody);

    return 0;
}


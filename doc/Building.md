Building from Source
===============================================================================

Prerequisites
-------------------------------------------------------------------------------
Building Fuel from source requires Qt version 4 or 5. Qt is available at:
	http://www.qt.io/download-open-source/

To run Fuel a compiled binary of Fossil must be available either in the system
path or in the same folder as the Fuel executable. You can find the latest
Fossil binaries from the Fossil homepage at:
	https://www.fossil-scm.org/download.html

Retrieving the source
-------------------------------------------------------------------------------
The source is available as a tar.gz archive at the following location

	https://fuel-scm.org/fossil/wiki?name=Downloads

Additionally you can clone the source code directly from our site using fossil

	mkdir fuel
	cd fuel
	fossil clone https://fuel-scm.org/fossil fuel.fossil
	fossil open fuel.fossil


Windows (Qt4 / MinGW)
-------------------------------------------------------------------------------
1. Open a Command Prompt and cd into the folder containing the Fuel source code

		cd fuel

2. Make a build folder and cd into it

		md build
		cd build

3. Generate the makefile with qmake

		C:\QtSDK\Desktop\Qt\4.8.1\mingw\bin\qmake ..\fuel.pro -r -spec win32-g++ CONFIG+=release

4. Build the project

		c:\QtSDK\mingw\bin\mingw32-make

5. Copy the Qt DLLs

		copy C:\QtSDK\Desktop\Qt\4.8.1\mingw\bin\QtCore4.dll release
		copy C:\QtSDK\Desktop\Qt\4.8.1\mingw\bin\QtGui4.dll release

6. Copy the MinGW DLLs

		copy C:\QtSDK\mingw\bin\libgcc_s_dw2-1.dll release
		copy C:\QtSDK\mingw\bin\mingwm10.dll release


Windows (Qt4 / MSVC)
-------------------------------------------------------------------------------
1. Open a Command Prompt and cd into the folder containing the Fuel source code

		cd fuel

2. Make a build folder and cd into it

		md build
		cd build

3. Generate the Visual Studio project makefile with qmake

		C:\QtSDK\Desktop\Qt\4.8.1\msvc2010\bin\qmake ..\fuel.pro -tp vc -spec win32-msvc2010

4. Open the generated project

		start fuel.vcxproj

5. Build the project
	Use the IDE to build the project or alternatively you can use via MSBuild

		c:\Windows\Microsoft.NET\Framework\v4.0.30319\msbuild Fuel.vcxproj /t:build /p:Configuration=Release

6. Copy the Qt DLLs

		copy C:\QtSDK\Desktop\Qt\4.8.1\msvc2010\bin\QtCore4.dll release
		copy C:\QtSDK\Desktop\Qt\4.8.1\msvc2010\bin\QtGui4.dll release

4. Enjoy

		release\Fuel.exe


Mac OS X
-------------------------------------------------------------------------------
Build Steps:

1. Open a Terminal and add your Qt bin folder to the path

		export PATH=$PATH:/path/to/qt/version/clang_64/bin

2. Go into the folder containing the Fuel source code

		cd fuel

3. Generate localization files

		intl/convert.sh

4. Generate the makefile with qmake

		qmake fuel.pro -spec macx-clang CONFIG+=release

5. Build the project

		make

6. (Optional) Include the Fossil executable within the Fuel application bundle

		cp /location/to/fossil Fuel.app/Contents/MacOS

7. Package Qt dependencies into Fuel to make a standalone application bundle

		macdeployqt Fuel.app

8. Enjoy

		open Fuel.app


Unix-based OS
-------------------------------------------------------------------------------
Build Steps:

1. cd into the folder containing the Fuel source code

		cd fuel

2. Generate localization files

		intl/convert.sh

3. Generate the makefile with qmake

		qmake fuel.pro

4. Build the project

		make

5. Enjoy

		./Fuel


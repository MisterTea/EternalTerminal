set VERSION=4.15

set SEVENZIP="C:\Program Files\7-Zip\7z.exe"

FOR /F "tokens=*" %%G IN ('DIR /AD /B /S Debug*') DO (
    DEL /S /Q "%%G"
    RD "%%G"
)
FOR /F "tokens=*" %%G IN ('DIR /AD /B /S Release*') DO (
    DEL /S /Q "%%G"
    RD "%%G"
)
DEL /Q "SimpleIni.ncb"
ATTRIB -H "SimpleIni.suo"
DEL /Q "SimpleIni.suo"
DEL /Q "SimpleIni.opt"
DEL /Q testsi-out*.ini
DEL /Q test1-blah.ini
DEL /Q test1-output.ini
START "Generate documentation" /WAIT "C:\Program Files (x86)\doxygen\bin\doxygen.exe" SimpleIni.doxy
cd ..
del simpleini-%VERSION%.zip
%SEVENZIP% a -tzip -r- -x!simpleini\.svn simpleini-%VERSION%.zip simpleini\*
del simpleini-doc.zip
%SEVENZIP% a -tzip -r simpleini-doc.zip simpleini-doc\*
cd simpleini

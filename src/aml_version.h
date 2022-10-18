#ifndef __AML_VERSION_H__
#define __AML_VERSION_H__

#ifdef  __cplusplus
extern "C" {
#endif

const char libVersion[]=
"MM-module-name:avsync-lib,version:1.2.86-gaa8c408";

const char libFeatures[]=
"MM-module-feature: support amaster,vmaster,pcr,freerun,mono mode for av sync mode \n" \
"MM-module-feature: support audio track switching \n" \
"MM-module-feature: config video delay time \n" \
"MM-module-feature: pause with special pts \n" \
"MM-module-feature: speed changed(0.25,0.5,1,1.25,1.5,2,3) \n" \
"MM-module-feature: config audio wait video time \n";

#ifdef  __cplusplus
}
#endif
#endif /*__AML_VERSION_H__*/
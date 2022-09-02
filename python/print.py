# -*- coding: utf-8 -*-
"""
Created on Thu Sep  1 17:15:50 2022

@author: Andy
"""

f = open("2.txt" , encoding = "utf-8")

lines = f.readlines()

dict1 = {}
 
for line in lines:#每次读文件中的一行
    
    if "__uvm_kvmalloc__" in line:
        #print(line)
        print("{}次输出__uvm_kvmalloc__：\n size:{} \n file:{} \n line:{} \n function:{}".format(
                                                  line[line.find("第"):line.find("次")-1],
                                                  line[line.find(":")+1:line.find("--file")],
                                                  line[line.find("--file")+7:line.find("--line:")],
                                                  line[line.find("--line:")+7:line.find("--function")],
                                                  line[line.find("--function")+10:],
                                                  )
              )
        
        
        
        #在字典中计数
        if line[line.find("--function")+10:] in dict1:
            dict1[line[line.find("--function")+10:]] += 1
        else:
            dict1[line[line.find("--function")+10:]] = 0

        continue
    
    
    '''
    if "__uvm_kvmalloc_zero__ " in line:
        #print(line)
        print("第{}次输出__uvm_kvmalloc_zero__：{}".format(line[line.find("第"):line.find("次")-1],line[line.find("，")+1:]))
        continue
    
    if "alloc_internal__" in line:
        #print(line)
        print("第{}次输出alloc_internal__：{}".format(line[line.find("第"):line.find("次")-1],line[line.find("，")+1:]))
        continue
    '''
    
    
    
    
    
    '''
    if "__uvm_kvmalloc--function" in line:
        print(line[line.find('第'):line.find('，')-1])
        print(line[line.find("出")+1:])
        continue
    '''
count = 1
for k,s in dict1.items():
    
    print ('{}被调用{}次'.format(k,s))
    count += 1
    
    
print("一共有{}个函数调用__uvm_kvmalloc".format(count))
    

f.close()

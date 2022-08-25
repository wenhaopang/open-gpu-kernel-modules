# -*- coding: utf-8 -*-
"""
Created on Wed Aug 24 15:16:09 2022

@author: Andy
"""
f = open("1.txt" , encoding = "utf-8")

lines = f.readlines()

dict1 = {}
 
for line in lines:#每次读文件中的一行
    
    #print(line)
    
    flag=1
    
    s1 = ""
    for l in line:#每次读一行中的每个字符
        #下面的操作就是将文件中调用的函数提取出来
        if l != ']' and flag:
            continue
        
        flag = 0
        if l == '被':
            break
        
        if l != ']' and l != ' ':#只提取'] '之后和'被'之前的字符串，即调用的函数
            s1 += l
        
        #print(l)
        
    #在字典中计数
    if s1 in dict1:
        dict1[s1] += 1
    else:
        dict1[s1] = 0
    #print(s1)
#输出调用哪些函数并输出调用次数
for k,s in dict1.items():
    print ('{}被调用{}次'.format(k,s))

    
f.close()

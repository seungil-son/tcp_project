원격 온습도 관리 웹 애플리케이션
---------------------------------------------------------

1.	개발 도구 : Arduino, Maria DB, Raspberry Pi, Ubuntu
2.	개발 언어 : HTML, C  
3.	설명:
   
Arduino
 
    -  DHT11센서를 사용한 데이터수집 
    -  LCD모니터에 로컬로 온 습도 표시
    -  모터 작동 대기
	
 Raspberry Pi 

    –  MariaDB를 이용한 데이터 저장
    -	Arduino와 시리얼 통신
    -	Ubuntu와 TCP통신

Ubuntu 
    
    - Raspberry Pi로부터 데이터를 수신
    - 웹 애플리케이션에 실시간 온 습도 변화 그래프 표시
    - 웹 애플리케이션에서 Arduino의 모터를 사용자 임의로 조작가능
    - 웹 애플리케이션에서 Ubuntu가 수신한 데이터를 일별 요약테이블을 통해 최대, 최소, 평균 온도, 평균 습도를 표시 

결과 

1. 웹 이미지
   
<img width="1901" height="890" alt="Image" src="https://github.com/user-attachments/assets/0c98f03e-5ccb-4769-9bb4-fdba3b4e4227" />

3. 아두이노

![Image](https://github.com/user-attachments/assets/f66873ba-bb45-49a8-b6c2-bfd820ba9370)

5. 개선점
   
   	 – 모터 제어 : 사용자가 임의로 설정한 값에 따라 자동으로 변경
   
       	 - 일별 요약 : 사용자가 요약표에서 보고싶은 값을 설정 가능하게 변경
   
       	 - 실시간 온 습도 수치 제공 : 그래프 만으로는 실시간 온 습도를 파악하는데 가시성이
                                    좋지 않음

 

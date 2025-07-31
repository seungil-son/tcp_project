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


 

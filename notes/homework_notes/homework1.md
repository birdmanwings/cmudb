# 前言

好好学一次数据库，这里准备的是CMU15445的课程，这里做的是2018秋季的，先是homework1，写十个sql。

[链接](https://15445.courses.cs.cmu.edu/fall2018/homework1/)

# 正文

## Sql1

送的不用管。

```sql lite
select count(distinct(city)) from station;s
```

## Sql2

```sql lite
select city,count(*) as cnt from station group by city order by cnt ASC,city ASC;

Palo Alto|5
Mountain View|7
Redwood City|7
San Jose|16
San Francisco|35
```

## Sql3

```sql lite
select city, (ROUND(city_trip_cnt.cnt * 1.0 / trip_cnt.cnt, 4)) as ratio from (select city, count(distinct(id)) as cnt from trip, station where trip.start_station_id = station_id or trip.end_station_id = station_id group by city) as city_trip_cnt, (select count(1) as cnt from trip) as trip_cnt order by ratio DESC, city ASC;

San Francisco|0.9011
San Jose|0.0566
Mountain View|0.0278
Palo Alto|0.0109
Redwood City|0.0052
```

佛了，第三题就那么绕，这里我们需要计算属于每个城市的旅行站总的旅行的比例，那么这里我们分块来算，首先肯定要知道总的旅行数量这个简单`select count(1) as cnt from trip`,然后是每个城市的旅行的数量，这里的定义是如果旅行的起发地或者结束地是这个城市，那么这次旅行就属于这个城市，也就是说一个旅行可能属于两个城市，注意这里如果起发地和结束地是一个地方的话只算一次，所以要用`distinct`，所以我们需要一个城市-旅行id的结果集`select city, count(distinct(id)) as cnt from trip, station where trip.start_station_id = station_id or trip.end_station_id = station_id group by city`，最后用每个城市旅行的数量city_trip_cnt除以总的旅行数量，保留四位小数。

## Sql4

```sqlite
select city, station_name, MAX(trip_cnt) from (select city, station_name, count(distinct(id)) as trip_cnt from trip, station where trip.start_station_id = station_id or trip.end_station_id = station_id group by station_name) group by city order by city ASC;

Mountain View|Mountain View Caltrain Station|12735
Palo Alto|Palo Alto Caltrain Station|3534
Redwood City|Redwood City Caltrain Station|2654
San Francisco|San Francisco Caltrain (Townsend at 4th)|111738
San Jose|San Jose Diridon Caltrain Station|18782
```

这个跟上面类似，我们要找出每个城市中最受欢迎的站点，那么我们首先需要得到每个城市每个站点的数量的集合，类似city-station_name-trip_cnt，然后在算每个城市中旅游次数最多的站点是哪个，计算每个城市每个站点的旅游数量`select city, station_name, count(distinct(id)) as trip_cnt from trip, station where trip.start_station_id = station_id or trip.end_station_id = station_id group by station_name, city`记得`group by station_id`要按站点分类，然后跟第三题一样计算每站的旅行数量，最后按城市分类`group by city`来计算每个城市最受欢迎的站点，按照城市升序排列。

## Sql5

```sqlite
with dates as (
    select date(start_time) as tdate
    from trip
    union
    select date(end_time) as tdate
    from trip where bike_id <= 100)

select tdate,
       round(sum(strftime('%s', min(datetime(end_time), datetime(tdate, '+1 day')))
               - strftime('%s', max(datetime(start_time), datetime(tdate))))
           * 1.0 / (select count(distinct(bike_id)) from trip where bike_id <= 100),
        4) as avg_duration
from trip, dates
where bike_id <= 100 and
      datetime(start_time) < datetime(tdate, '+1 day') and
      datetime(end_time) > datetime(tdate)
group by tdate
order by avg_duration desc limit 10;

2014-07-13|3884.1758
2014-10-12|3398.9011
2015-02-14|2728.3516
2014-08-29|2669.011
2015-07-04|2666.3736
2014-06-23|2653.1868
2013-10-01|2634.7253
2014-05-18|2618.2418
2015-02-15|2582.6374
2014-10-11|2555.6044
```

这题应该是最难的了，需要仔细分析下，首先我们看下两个函数的用法

- [http://nethelp.wikidot.com/date-add-subtract-in-sqlite](https://links.jianshu.com/go?to=http%3A%2F%2Fnethelp.wikidot.com%2Fdate-add-subtract-in-sqlite)
- [https://www.w3resource.com/sqlite/sqlite-strftime.php](https://links.jianshu.com/go?to=https%3A%2F%2Fwww.w3resource.com%2Fsqlite%2Fsqlite-strftime.php)

首先我们需要得到start_time和end_time的日期的集合，这里可以用下with as语句（子查询）

`with dates as (select date(start_time) as tdate from trip union select date(end_time) as tdate from trip bike_id <= 100)`

因为我们只需要统计出发或者结束的日期，注意这里用union可以去重。

接着我们需要分析一下The average bike utilization的定义，对于除数很简单就是`select conut(distinct(bike_id)) from trip where bike_id <= 100`，然后分析下被除数应该为分为被减数（结束时间）和减数（开始时间），由题目的条件可知我们需要对这两个时间和那日的时间进行一下比较，对于某个tdata，当end_time < tdate+1的时间时取end_time，当end_time > tdate+1的时间时我们应该取tdate+1，所以我们应该用min()，同理对于开始时间我们就应该取较大的要用max，所以最后被除数应该就是

`strftime('%s', min(datetime(end_time), datetime(tdate, '+1 day'))) - strftime('%s', max(datetime(start_time), datetime(tdate))))`

最后我们可以得出这个utilization

```sqlite
round(sum(strftime('%s', min(datetime(end_time), datetime(tdate, '+1 day')))
               - strftime('%s', max(datetime(start_time), datetime(tdate))))
           * 1.0 / (select count(distinct(bike_id)) from trip where bike_id <= 100),
        4) as avg_duration
```

注意我们这里来到了这题的关键所在，我们现在要将trip表和我们之前定义的dates联合起来（这是类似一个笛卡尔积），每一行的trip都会与每一个tdate进行组合，但是这里我们要注意，只有当start_time和end_time这个区域的一部分属于[tdate,tdate+1]之间时才是符合条件的（否则tdate就不再这个trip的时间中，上面计算的utilization就没有意义了），也就是

```sqlite
datetime(start_time) < datetime(tdate, '+1 day') and
      datetime(end_time) > datetime(tdate)
```

当然别忘了还有个限制条件bike_id <= 100，然后按照tdate分类，按照avg_duration降序，limit取前十

## Sql6

```sqlite
with use_trip as (select * from trip where bike_id >= 100 and bike_id <= 200)
select former_trip.bike_id, former_trip.id, former_trip.start_time, former_trip.end_time, latter_trip.id, latter_trip.start_time, latter_trip.end_time from use_trip as former_trip, use_trip as latter_trip where former_trip.bike_id = latter_trip.bike_id and former_trip.id < latter_trip.id and former_trip.start_time < latter_trip.end_time and former_trip.end_time > latter_trip.start_time order by former_trip.bike_id ASC, former_trip.id ASC, latter_trip.id ASC;

144|815060|2015-06-19 21:26:00|2015-06-19 22:17:00|815073|2015-06-19 22:10:00|2015-06-19 22:17:00
158|576536|2014-12-15 15:05:00|2014-12-15 23:11:00|576591|2014-12-15 16:07:00|2014-12-15 16:17:00
158|576536|2014-12-15 15:05:00|2014-12-15 23:11:00|576604|2014-12-15 16:28:00|2014-12-15 16:40:00
```

这题比上一题简单很多，题目要求我们查找存在错误的数据，当一辆自行车被两个旅行同时使用时，就是错误。首先我们用with as选出bike_id在100到200之间的trip方便后面的使用。然后我们需要用两个相同的表来查，所以将use_trip定义为former_trip和latter_trip然后按照要求打印就行了，最后加上限制条件两个的bike_id相等，时间有重叠，只打印前面一个的结果，然后按照要求排序就行了。

## Sql7

```sqlite
select bike_id, count(distinct city) as cnt from trip, station where start_station_id = station_id or end_station_id = station_id group by bike_id having cnt > 1 order by cnt DESC, bike_id ASC;

15|5
25|5
27|5
31|5
43|5
51|5
...
664|2
665|2
666|2
687|2
697|2
709|2
717|2
```

比较简单，别忘了having cat > 1的限定条件哦

## Sql8

```sqlite
with use_weather as (select count(distinct date) as weather_cnt, events from weather group by events)
select w.events, ROUND(count(t.id) * 1.0 / weather_cnt, 4) as avg_trips from trip as t, station as s, weather as w, use_weather where t.start_station_id = s.station_id and s.zip_code = w.zip_code and w.events = use_weather.events and date(t.start_time) = date group by w.events order by avg_trips DESC, w.events ASC;

\N|801.0528
Fog|682.4375
Fog-Rain|573.0833
Rain|560.0394
Rain-Thunderstorm|491.6667
rain|488.0
```

我们先用with as得到一个每种天气的天数，然后就是正常按照条件搜索，注意要限制好旅行的地区，从而对应的天气。

## Sql9

```sqlite
with short_trip as (
select ROUND(avg(mean_temp), 4) from trip t, station s, weather w where t.start_station_id = s.station_id and s.zip_code = w.zip_code and w.date = date(t.start_time) and strftime('%s',datetime(t.end_time)) -strftime('%s',datetime(t.start_time)) <= 60
),
long_trip as (
select round(avg(mean_temp),4) from trip t, station s, weather w
where t.start_station_id = s.station_id and s.zip_code = w.zip_code 
and w.date = date(t.start_time) and
strftime('%s',datetime(t.end_time)) - strftime('%s',datetime(t.start_time)) > 60
)
select * from short_trip, long_trip;

60.9391|61.2227
```

要求输出在同一行，那么我么可以用子查询的方法来把它分成两个部分。

## Sql10

```sqlite
select s.zip_code, s.station_name, count(distinct t.id) as cnt from weather w, station s, trip t where w.events = 'Rain-Thunderstorm' and w.date = date(t.start_time) and t.start_station_id = s.station_id and s.zip_code = w.zip_code group by s.zip_code, s.station_name order by cnt DESC limit 1;

94107|San Francisco Caltrain (Townsend at 4th)|114
```

# 总结

homework的sql太复杂了，一般也不会有人这么写吧。。。好的继续学习了
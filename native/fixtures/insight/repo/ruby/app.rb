require_relative 'lib/thing'
require 'json'

class App
  def initialize(name)
    @name = name
  end

  def greet(times)
    if times > 1
      times.times do |i|
        puts "#{@name} #{i}" unless i.zero?
      end
    elsif times == 1
      puts @name
    end
  end
end

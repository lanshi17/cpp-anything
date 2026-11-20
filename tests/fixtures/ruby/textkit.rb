# Count the words in text.
def word_count(text)
  text.split(/\s+/).reject(&:empty?).length
end

# Count the digit characters.
def digits_only(text)
  text.count("0-9")
end

# Repeat a needle inside text.
def repeat_count(text, needle)
  text.scan(needle).length
end

# Average word length, zero for empty.
def average_word_length(text)
  words = text.split(/\s+/).reject(&:empty?)
  return 0.0 if words.empty?
  words.sum(&:length).to_f / words.length
end
